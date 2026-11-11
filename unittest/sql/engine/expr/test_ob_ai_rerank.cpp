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

#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_expr_ai_rerank.h"

using namespace oceanbase::common;
using oceanbase::sql::ObExprAIRerank;

static ObJsonArray *parse_array(ObIAllocator &allocator, const char *json)
{
  ObString input(json);
  ObIJsonBase *document = nullptr;
  const int ret = ObJsonBaseFactory::get_json_base(
      &allocator, input, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, document);
  return ret == OB_SUCCESS && document != nullptr && document->json_type() == ObJsonNodeType::J_ARRAY
      ? static_cast<ObJsonArray *>(document) : nullptr;
}

TEST(AIRerank, prepares_homogeneous_inputs)
{
  ObArenaAllocator allocator;
  ObJsonArray *texts = nullptr;
  ObString key("text");
  auto *strings = parse_array(allocator, R"(["first","second"])");
  ASSERT_EQ(OB_SUCCESS, ObExprAIRerank::prepare_documents(allocator, strings, nullptr, texts));
  ASSERT_EQ(2, texts->element_count());
  EXPECT_EQ(5, texts->get_value(0)->get_data_length());
  auto *objects = parse_array(allocator, R"([{"text":"first","id":"one"},{"text":"second","id":2}])");
  ASSERT_EQ(OB_SUCCESS, ObExprAIRerank::prepare_documents(allocator, objects, &key, texts));
  ASSERT_EQ(2, texts->element_count());
  EXPECT_EQ(6, texts->get_value(1)->get_data_length());

  for (const char *json : {R"([])", R"(["", "second"])", R"([null])", R"(["text",{}])"}) {
    EXPECT_EQ(OB_INVALID_ARGUMENT,
        ObExprAIRerank::prepare_documents(allocator, parse_array(allocator, json), nullptr, texts));
    EXPECT_EQ(nullptr, texts);
  }
  for (const char *json : {R"([{}])", R"([{"text":2}])", R"([{"text":""}])", R"([{"text":null}])"}) {
    EXPECT_EQ(OB_INVALID_ARGUMENT,
        ObExprAIRerank::prepare_documents(allocator, parse_array(allocator, json), &key, texts));
    EXPECT_EQ(nullptr, texts);
  }
}

TEST(AIRerank, validates_response_before_mapping)
{
  ObArenaAllocator allocator;
  for (const char *bad : {
      R"([{"index":0,"relevance_score":0.9}])",
      R"([{"index":0,"relevance_score":0.9},{"index":0,"relevance_score":0.8}])",
      R"([{"index":0,"relevance_score":0.9},{"index":2,"relevance_score":0.8}])",
      R"([{"index":0,"relevance_score":0.9},{"index":-1,"relevance_score":0.8}])",
      R"([{"index":0,"relevance_score":0.9},{"relevance_score":0.8}])",
      R"([{"index":0,"relevance_score":0.9},{"index":1.5,"relevance_score":0.8}])",
      R"([{"index":0,"relevance_score":0.9},{"index":1,"relevance_score":"0.8"}])"}) {
    ObArray<ObExprAIRerank::RerankHit> hits;
    auto *batch = parse_array(allocator, bad);
    ASSERT_NE(nullptr, batch);
    EXPECT_EQ(OB_INVALID_DATA, ObExprAIRerank::collect_batch_hits(batch, 0, 2, hits));
  }
}

TEST(AIRerank, stable_global_top_n_preserves_properties)
{
  ObArenaAllocator allocator;
  auto *docs = parse_array(allocator, R"([{"id":"x"},{"id":"y","tag":7},{"id":"z","tag":8}])");
  auto *first = parse_array(allocator, R"([{"index":0,"relevance_score":0},{"index":1,"relevance_score":1}])");
  auto *second = parse_array(allocator, R"([{"index":0,"relevance_score":1}])");
  ObArray<ObExprAIRerank::RerankHit> hits;
  ASSERT_EQ(OB_SUCCESS, ObExprAIRerank::collect_batch_hits(first, 0, 2, hits));
  ASSERT_EQ(OB_SUCCESS, ObExprAIRerank::collect_batch_hits(second, 2, 1, hits));
  ObJsonArray *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObExprAIRerank::build_rerank_result(allocator, docs, true, 2, hits, result));
  ASSERT_EQ(2, result->element_count());
  auto *top = static_cast<ObJsonObject *>(result->get_value(0));
  auto *next = static_cast<ObJsonObject *>(result->get_value(1));
  ASSERT_NE(nullptr, top->get_value("id"));
  ASSERT_NE(nullptr, next->get_value("id"));
  EXPECT_EQ('y', top->get_value("id")->get_data()[0]);
  EXPECT_EQ('z', next->get_value("id")->get_data()[0]);
  ASSERT_NE(nullptr, next->get_value("tag"));
  EXPECT_EQ(8, next->get_value("tag")->get_int());
  ASSERT_NE(nullptr, next->get_value("_model_score"));
  EXPECT_EQ(1, next->get_value("_model_score")->get_int());

  ASSERT_EQ(OB_SUCCESS, ObExprAIRerank::build_rerank_result(allocator, docs, false, 2, hits, result));
  auto *global = static_cast<ObJsonObject *>(result->get_value(1));
  ASSERT_NE(nullptr, global->get_value("index"));
  EXPECT_EQ(2, global->get_value("index")->get_int());
}
