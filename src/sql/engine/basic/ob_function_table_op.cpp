/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/schema/ob_schema_utils.h"
#include "share/ob_server_struct.h"
#include "sql/pl/ob_pl_user_type.h"
#include <new>


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    file_scan_spec_, file_projection_);

int get_file_sql_root(ObSQLSessionInfo &session, ObIAllocator &alloc, ObString &root)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL while enable_file_sql is OFF");
  } else if (!(session.get_user_priv_set() & OB_PRIV_FILE)) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
              *GCTX.schema_service_, share::SYS_VAR_SECURE_FILE_PRIV, alloc, root))) {
  } else if (root.empty() || 0 == root.case_compare("NULL")) {
    ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }
  return ret;
}

int file_sql_status(file_sql::Status status, const std::string &message)
{
  int ret = OB_SUCCESS;
  switch (status) {
    case file_sql::Status::OK: break;
    case file_sql::Status::END: ret = OB_ITER_END; break;
    case file_sql::Status::DENIED: ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS; break;
    case file_sql::Status::IO: ret = OB_IO_ERROR; break;
    case file_sql::Status::EXISTS: ret = OB_FILE_ALREADY_EXIST; break;
    case file_sql::Status::INVALID:
    case file_sql::Status::CHANGED:
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.c_str());
      break;
    case file_sql::Status::UNSUPPORTED:
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, message.c_str());
      break;
  }
  if (ret != OB_SUCCESS && ret != OB_ITER_END) {
    LOG_WARN("file SQL operation failed", K(ret), "detail", message.c_str());
  }
  return ret;
}

int ObFunctionTableOp::open_file_scan()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator path_allocator("FileSqlPath");
  ObString root;
  file_sql::ScanSpec spec;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  file_reader_.reset();
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(get_file_sql_root(*session, path_allocator, root))) {
  } else if (!spec.decode(MY_SPEC.file_scan_spec_.ptr(), MY_SPEC.file_scan_spec_.length())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    std::vector<int64_t> projection;
    for (int64_t i = 0; i < MY_SPEC.file_projection_.count(); ++i) {
      projection.push_back(MY_SPEC.file_projection_.at(i));
    }
    file_reader_.reset(new (std::nothrow) file_sql::Reader());
    if (OB_ISNULL(file_reader_.get())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      const auto status = file_reader_->open(std::string(root.ptr(), root.length()), spec, projection);
      ret = file_sql_status(status, file_reader_->error());
    }
  }
  if (OB_FAIL(ret)) file_reader_.reset();
  return ret;
}

int ObFunctionTableOp::inner_get_next_file_row()
{
  size_t bytes = 0;
  clear_evaluated_flag();
  return read_file_row(bytes);
}

int ObFunctionTableOp::inner_get_next_batch(int64_t max_row_cnt)
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  brs_.size_ = 0;
  brs_.end_ = false;
  brs_.skip_->reset(max_row_cnt);
  ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx_);
  guard.set_batch_size(max_row_cnt);
  size_t bytes = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < max_row_cnt && bytes < 4 * 1024 * 1024; ++i) {
    guard.set_batch_idx(i);
    if (OB_FAIL(read_file_row(bytes))) {
      if (ret == OB_ITER_END) {
        ret = OB_SUCCESS;
        brs_.end_ = true;
        break;
      }
    } else {
      ++brs_.size_;
    }
  }
  return ret;
}

int ObFunctionTableOp::read_file_row(size_t &bytes)
{
  int ret = OB_SUCCESS;
  std::vector<file_sql::Cell> row;
  if (OB_ISNULL(file_reader_.get())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    const auto status = file_reader_->next(row);
    ret = file_sql_status(status, file_reader_->error());
  }
  CK (row.size() == static_cast<size_t>(MY_SPEC.column_exprs_.count()));
  for (size_t i = 0; OB_SUCC(ret) && i < row.size(); ++i) {
    const ObExpr &expr = *MY_SPEC.column_exprs_.at(i);
    ObDatum &out = expr.locate_datum_for_write(eval_ctx_);
    const auto &cell = row[i];
    bytes += cell.text.size();
    int64_t integer = 0;
    double real = 0;
    if (cell.type == file_sql::Type::NIL) {
      out.set_null();
    } else {
      switch (expr.datum_meta_.type_) {
        case ObIntType:
          if (!file_sql::parse_integer(cell.text, integer)) ret = OB_ERR_UNEXPECTED;
          else out.set_int(integer);
          break;
        case ObDoubleType:
          if (!file_sql::parse_double(cell.text, real)) ret = OB_ERR_UNEXPECTED;
          else out.set_double(real);
          break;
        case ObTinyIntType:
          out.set_int(cell.text == "true" || 0 == ObString(cell.text.size(), cell.text.data()).case_compare("true"));
          break;
        case ObDateType:
        case ObDateTimeType:
          if (!file_sql::parse_temporal(cell.text, integer)) ret = OB_ERR_UNEXPECTED;
          else if (expr.datum_meta_.type_ == ObDateType) out.set_date(integer / 86400000000LL);
          else out.set_datetime(integer);
          break;
        case ObVarcharType: {
          if (cell.text.size() > OB_MAX_VARCHAR_LENGTH) {
            ret = OB_SIZE_OVERFLOW;
          } else {
            char *buf = expr.get_str_res_mem(eval_ctx_, cell.text.size() + 1);
            if (OB_ISNULL(buf)) ret = OB_ALLOCATE_MEMORY_FAILED;
            else {
              MEMCPY(buf, cell.text.data(), cell.text.size());
              out.set_string(buf, cell.text.size());
            }
          }
          break;
        }
        default:
          ret = OB_ERR_UNEXPECTED;
          break;
      }
    }
    if (OB_SUCC(ret)) expr.set_evaluated_projected(eval_ctx_);
  }
  return ret;
}

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (!MY_SPEC.file_scan_spec_.empty()) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_file_row;
    OZ (open_file_scan());
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (!MY_SPEC.file_scan_spec_.empty()) {
    OZ (open_file_scan());
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  file_reader_.reset();
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  return ret;
}

void ObFunctionTableOp::destroy()
{
  file_reader_.reset();
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
