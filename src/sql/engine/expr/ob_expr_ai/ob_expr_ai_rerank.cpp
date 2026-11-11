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
#include "ob_expr_ai_rerank.h"
#include <algorithm>
#include <cmath>
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 5)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num >= 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret) && param_num == 5) {
      const ObObjType top_n_type = types_stack[TOP_N_IDX].get_type();
      if (!ob_is_integer_type(top_n_type)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, top_n must be an integer");
      } else {
        types_stack[TOP_N_IDX].set_calc_type(ob_is_uint_tc(top_n_type) ? ObUInt64Type : ObIntType);
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  ObDatum *arg_top_n = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key, arg_top_n))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id, query and documents must not be null");
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObString doc_key;
    const bool object_mode = OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null();
    ObIJsonBase *documents = nullptr;
    bool is_null = false;
    ObJsonArray *texts = nullptr;
    ObJsonArray *original = nullptr;
    uint64_t top_n = 0;
    if (model_id.empty() || query.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id and query must not be empty");
    } else if (object_mode && OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator,
                   *arg_doc_key, expr.args_[DOC_KEY_IDX]->datum_meta_,
                   expr.args_[DOC_KEY_IDX]->obj_meta_.has_lob_header(), doc_key))) {
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, allocator, DOCUMENTS_IDX,
                                                     documents, is_null))) {
    } else if (is_null || OB_ISNULL(documents) || documents->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, documents must be a nonempty array");
    } else {
      original = static_cast<ObJsonArray *>(documents);
      top_n = original->element_count();
      if (OB_NOT_NULL(arg_top_n)) {
        top_n = 0;
        if (!arg_top_n->is_null()) {
          if (ob_is_uint_tc(expr.args_[TOP_N_IDX]->datum_meta_.type_)) {
            top_n = arg_top_n->get_uint();
          } else if (arg_top_n->get_int() > 0) {
            top_n = static_cast<uint64_t>(arg_top_n->get_int());
          }
        }
        if (top_n == 0 || top_n > original->element_count()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, top_n must be between 1 and the document count");
        }
      }
    }

    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint;
    auto *resolver = ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObArray<ObString> headers;
    ObArray<RerankHit> hits;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(prepare_documents(allocator, original, object_mode ? &doc_key : nullptr, texts))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_ISNULL(resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(resolver->resolve_by_model_name(model_id, allocator, endpoint))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_header(allocator, *info, endpoint, headers))) {
    } else {
      ObAIFuncClient client;
      for (uint64_t begin = 0; OB_SUCC(ret) && begin < texts->element_count();) {
        const uint64_t count = std::min<uint64_t>(RERANK_BATCH_SIZE, texts->element_count() - begin);
        ObJsonArray *batch = nullptr;
        ObJsonObject *config = nullptr;
        ObJsonInt *batch_top_n = nullptr;
        ObJsonObject *body = nullptr;
        ObJsonObject *response = nullptr;
        ObIJsonBase *ranked = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, batch))) {
        } else {
          for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
            ret = batch->append(texts->get_value(begin + i));
          }
        }
        // Score every document before applying the global limit.
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, count, batch_top_n))) {
        } else if (OB_FAIL(config->add("top_n", batch_top_n))) {
        } else if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, *info, endpoint, query, batch, config, body))) {
        } else if (OB_FAIL(client.send_post(allocator, endpoint.get_url(), headers, body, response))) {
        } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint, response, ranked))) {
        } else if (OB_ISNULL(ranked) || ranked->json_type() != ObJsonNodeType::J_ARRAY) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(collect_batch_hits(static_cast<ObJsonArray *>(ranked), begin, count, hits))) {
        }
        begin += count;
      }
    }

    ObJsonArray *result = nullptr;
    ObString raw_str;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(build_rerank_result(allocator, original, object_mode, top_n, hits, result))) {
    } else if (OB_FAIL(result->get_raw_binary(raw_str, &allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
    }
  }
  if (ret == OB_INVALID_DATA) {
    FORWARD_USER_ERROR(ret, "invalid rerank response");
  }
  return ret;
}

int ObExprAIRerank::prepare_documents(ObIAllocator &allocator, ObJsonArray *documents,
                                      const ObString *doc_key, ObJsonArray *&texts)
{
  INIT_SUCC(ret);
  texts = nullptr;
  ObJsonArray *values = nullptr;
  if (OB_ISNULL(documents) || documents->element_count() == 0 ||
      (OB_NOT_NULL(doc_key) && doc_key->empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, documents and document key must not be empty");
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, values))) {
  } else {
    for (uint64_t i = 0; OB_SUCC(ret) && i < documents->element_count(); ++i) {
      ObJsonNode *node = documents->get_value(i);
      if (OB_NOT_NULL(doc_key) && OB_NOT_NULL(node) && node->json_type() == ObJsonNodeType::J_OBJECT) {
        node = static_cast<ObJsonObject *>(node)->get_value(*doc_key);
      } else if (OB_NOT_NULL(doc_key)) {
        node = nullptr;
      }
      if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_STRING || node->get_data_length() == 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, each document must provide a nonempty string");
      } else {
        ObString value(node->get_data_length(), node->get_data());
        ObJsonString *text = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, value, text))) {
        } else if (OB_FAIL(values->append(text))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      texts = values;
    }
  }
  return ret;
}

int ObExprAIRerank::collect_batch_hits(ObJsonArray *batch, uint64_t base, uint64_t batch_size,
                                       ObArray<RerankHit> &hits)
{
  INIT_SUCC(ret);
  bool seen[RERANK_BATCH_SIZE] = {};
  if (OB_ISNULL(batch) || batch_size == 0 || batch_size > RERANK_BATCH_SIZE ||
      batch->element_count() != batch_size || base > INT64_MAX - batch_size) {
    ret = OB_INVALID_DATA;
  }
  for (uint64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
    ObJsonNode *entry = batch->get_value(i);
    if (OB_ISNULL(entry) || entry->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else {
      auto *response = static_cast<ObJsonObject *>(entry);
      const ObJsonNode *index_node = response->get_value(INDEX_KEY);
      const ObJsonNode *score_node = response->get_value(SCORE_KEY);
      uint64_t index = batch_size;
      if (OB_NOT_NULL(index_node)) {
        if (index_node->json_type() == ObJsonNodeType::J_INT && index_node->get_int() >= 0) {
          index = static_cast<uint64_t>(index_node->get_int());
        } else if (index_node->json_type() == ObJsonNodeType::J_UINT) {
          index = index_node->get_uint();
        }
      }
      double score = 0;
      if (index >= batch_size || seen[index] || OB_ISNULL(score_node)) {
        ret = OB_INVALID_DATA;
      } else {
        switch (score_node->json_type()) {
          case ObJsonNodeType::J_INT:
          case ObJsonNodeType::J_UINT:
          case ObJsonNodeType::J_DOUBLE:
          case ObJsonNodeType::J_DECIMAL:
            if (OB_FAIL(score_node->to_double(score)) || !std::isfinite(score)) {
              ret = OB_INVALID_DATA;
            }
            break;
          default:
            ret = OB_INVALID_DATA;
            break;
        }
        if (OB_SUCC(ret)) {
          seen[index] = true;
          ret = hits.push_back(RerankHit{base + index, score, hits.count(), response});
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::build_rerank_result(ObIAllocator &allocator, ObJsonArray *documents,
                                        bool object_mode, uint64_t top_n, ObArray<RerankHit> &hits,
                                        ObJsonArray *&result)
{
  INIT_SUCC(ret);
  result = nullptr;
  ObJsonArray *output = nullptr;
  if (OB_ISNULL(documents) || top_n == 0 || top_n > static_cast<uint64_t>(hits.count()) ||
      static_cast<uint64_t>(hits.count()) != documents->element_count()) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, output))) {
  } else {
    // Preserve provider order within a batch for equal scores and merge batches stably.
    RerankHit *first = &hits.at(0);
    std::sort(first, first + hits.count(), [](const RerankHit &a, const RerankHit &b) {
      return a.score_ == b.score_ ? a.sequence_ < b.sequence_ : a.score_ > b.score_;
    });
    for (uint64_t i = 0; OB_SUCC(ret) && i < top_n; ++i) {
      const RerankHit &hit = hits.at(i);
      ObJsonNode *source = object_mode ? documents->get_value(hit.index_) : hit.response_;
      ObJsonObject *entry = nullptr;
      if (OB_ISNULL(source) || source->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
      } else if (OB_ISNULL(entry = static_cast<ObJsonObject *>(source->clone(&allocator, true)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (object_mode) {
        ObJsonNode *score = hit.response_->get_value(SCORE_KEY)->clone(&allocator, true);
        if (OB_ISNULL(score)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(entry->add(MODEL_SCORE_KEY, score))) {
        }
      } else {
        ObJsonInt *index = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, hit.index_, index))) {
        } else if (OB_FAIL(entry->add(INDEX_KEY, index))) {
        }
      }
      if (OB_SUCC(ret)) {
        ret = output->append(entry);
      }
    }
    if (OB_SUCC(ret)) {
      result = output;
    }
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
