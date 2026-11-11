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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        const ObObjType dim_type = types_stack[DIM_IDX].get_type();
        types_stack[DIM_IDX].set_calc_type(ob_is_uint_tc(dim_type) ? ObUInt64Type : ObIntType);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

static int get_embedding_dimension(const ObDatum *arg_dim, ObObjType dim_type, int64_t &dimension)
{
  int ret = OB_SUCCESS;
  dimension = 0;
  if (OB_NOT_NULL(arg_dim)) {
    if (!arg_dim->is_null()) {
      if (ob_is_uint_tc(dim_type)) {
        const uint64_t value = arg_dim->get_uint();
        if (value <= static_cast<uint64_t>(INT64_MAX)) {
          dimension = static_cast<int64_t>(value);
        }
      } else {
        dimension = arg_dim->get_int();
      }
    }
    if (dimension <= 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension must be a positive signed 64-bit integer");
    }
  }
  return ret;
}

static int request_embeddings(ObIAllocator &allocator, const ObString &model_id, int64_t dimension,
                              ObArray<ObString> &inputs, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint;
  auto *resolver = ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  ObJsonObject *config = nullptr;
  ObJsonInt *dim_json = nullptr;
  // An omitted dimension leaves the provider's default in effect.
  if (dimension > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension, dim_json))) {
    } else if (OB_FAIL(config->add("dimensions", dim_json))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
  } else if (OB_ISNULL(resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(resolver->resolve_by_model_name(model_id, allocator, endpoint))) {
  } else {
    ObAIFuncModel model(allocator, *info, endpoint);
    ret = model.call_dense_embedding_vector_v2(inputs, config, results);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *model_id = nullptr;
  ObDatum *content = nullptr;
  ObDatum *dimension = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, model_id, content, dimension))) {
  } else if (content->is_null()) {
    res.set_null();
  } else if (model_id->is_null() || model_id->get_string().empty() || content->get_string().empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
  } else {
    ObEvalCtx::TempAllocGuard guard(ctx);
    MultimodeAlloctor allocator(guard.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard{lib::ObMemAttr(N_AI_EMBED)};
    int64_t dim = 0;
    ObArray<ObString> inputs;
    ObArray<ObString> results;
    if (OB_FAIL(get_embedding_dimension(dimension,
            dimension == nullptr ? ObNullType : expr.args_[DIM_IDX]->datum_meta_.type_, dim))) {
    } else if (OB_FAIL(inputs.push_back(content->get_string()))) {
    } else if (OB_FAIL(request_embeddings(allocator, model_id->get_string(), dim, inputs, results))) {
    } else if (results.count() != 1) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, results.at(0)))) {
    }
  }
  return ret;
}

static constexpr int64_t EMBEDDING_BATCH_SIZE = 32;

struct EmbeddingBatchRow
{
  ObString model_id_;
  ObString text_;
  int64_t dimension_;
  int64_t row_;
};

static int flush_embedding_rows(const ObExpr &expr, ObEvalCtx &ctx,
                                const EmbeddingBatchRow *rows, int64_t count)
{
  int ret = OB_SUCCESS;
  bool grouped[EMBEDDING_BATCH_SIZE] = {};
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    if (!grouped[i]) {
      ObArray<ObString> inputs;
      int64_t row_indices[EMBEDDING_BATCH_SIZE];
      for (int64_t j = i; OB_SUCC(ret) && j < count; ++j) {
        if (!grouped[j] && rows[i].dimension_ == rows[j].dimension_ &&
            rows[i].model_id_ == rows[j].model_id_) {
          row_indices[inputs.count()] = rows[j].row_;
          if (OB_FAIL(inputs.push_back(rows[j].text_))) {
          } else {
            grouped[j] = true;
          }
        }
      }
      ObEvalCtx::TempAllocGuard guard(ctx);
      MultimodeAlloctor allocator(guard.get_allocator());
      ObArray<ObString> results;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(request_embeddings(
                     allocator, rows[i].model_id_, rows[i].dimension_, inputs, results))) {
      } else if (results.count() != inputs.count()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        for (int64_t j = 0; OB_SUCC(ret) && j < results.count(); ++j) {
          const ObString &value = results.at(j);
          const int64_t row = row_indices[j];
          char *buffer = expr.get_str_res_mem(ctx, value.length(), row);
          if (OB_ISNULL(buffer)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(buffer, value.ptr(), value.length());
            expr.locate_expr_datum(ctx, row).set_string(ObString(value.length(), buffer));
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                     const ObBitVector &skip, int64_t size)
{
  int ret = OB_SUCCESS;
  ObBitVector &evaluated = expr.get_evaluated_flags(ctx);
  for (int64_t arg = 0; OB_SUCC(ret) && arg < expr.arg_cnt_; ++arg) {
    ret = expr.args_[arg]->eval_batch(ctx, skip, size);
  }
  lib::ObMallocHookAttrGuard malloc_guard{lib::ObMemAttr(N_AI_EMBED)};
  EmbeddingBatchRow rows[EMBEDDING_BATCH_SIZE];
  int64_t count = 0;
  for (int64_t row = 0; OB_SUCC(ret) && row < size; ++row) {
    if (!skip.at(row) && !evaluated.at(row)) {
      const ObDatum &text = expr.args_[CONTENT_IDX]->locate_expr_datum(ctx, row);
      const ObDatum &model = expr.args_[MODEL_IDX]->locate_expr_datum(ctx, row);
      if (text.is_null()) {
        expr.locate_expr_datum(ctx, row).set_null();
      } else if (model.is_null() || model.get_string().empty() || text.get_string().empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      } else {
        const ObExpr *dim_expr = expr.arg_cnt_ == 3 ? expr.args_[DIM_IDX] : nullptr;
        const ObDatum *dim_arg = dim_expr == nullptr ? nullptr : &dim_expr->locate_expr_datum(ctx, row);
        int64_t dimension = 0;
        if (OB_FAIL(get_embedding_dimension(
                       dim_arg, dim_expr == nullptr ? ObNullType : dim_expr->datum_meta_.type_, dimension))) {
        } else {
          rows[count++] = EmbeddingBatchRow{model.get_string(), text.get_string(), dimension, row};
          if (count == EMBEDDING_BATCH_SIZE) {
            ret = flush_embedding_rows(expr, ctx, rows, count);
            count = 0;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && count != 0) {
    ret = flush_embedding_rows(expr, ctx, rows, count);
  }
  if (OB_SUCC(ret)) {
    for (int64_t row = 0; row < size; ++row) {
      if (!skip.at(row)) {
        evaluated.set(row);
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr, ObExpr &rt_expr) const
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
  rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  return OB_SUCCESS;
}

} // namespace sql
} // namespace oceanbase
