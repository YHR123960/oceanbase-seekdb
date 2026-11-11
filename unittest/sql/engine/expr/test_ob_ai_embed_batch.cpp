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
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

TEST(AIEmbedBatch, openai_reorders_indexed_response)
{
  ObArenaAllocator allocator;
  ObString response(R"({"data":[{"index":1,"embedding":[20,21]},{"index":0,"embedding":[10,11]}]})");
  ObJsonObject *object = nullptr;
  ObIJsonBase *result = nullptr;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, response, object));
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator, object, result));
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(2, result->element_count());
  for (uint64_t i = 0; i < 2; ++i) {
    ObIJsonBase *vector = nullptr;
    ObIJsonBase *first = nullptr;
    ASSERT_EQ(OB_SUCCESS, result->get_array_element(i, vector));
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::validate_embedding(vector, 2));
    ASSERT_EQ(OB_SUCCESS, vector->get_array_element(0, first));
    EXPECT_EQ(10 + i * 10, first->get_int());
  }
}

TEST(AIEmbedBatch, openai_rejects_invalid_indices)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (const char *json : {
      R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})",
      R"({"data":[{"index":0,"embedding":[1]},{"index":2,"embedding":[2]}]})",
      R"({"data":[{"index":0,"embedding":[1]},{"index":-1,"embedding":[2]}]})",
      R"({"data":[{"index":0,"embedding":[1]},{"embedding":[2]}]})",
      R"({"data":[{"index":0,"embedding":[1]},{"index":1.5,"embedding":[2]}]})",
      R"({"data":[]})"}) {
    ObString response(json);
    ObJsonObject *object = nullptr;
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, response, object));
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator, object, result));
    EXPECT_EQ(nullptr, result);
  }
}

TEST(AIEmbedBatch, ollama_accepts_multiple_inputs_and_preserves_position)
{
  ObArenaAllocator allocator;
  ObOllamaUtils::ObOllamaEmbed provider;
  ObString model("test-model");
  ObArray<ObString> inputs;
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("first")));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("second")));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, inputs, nullptr, body));
  ASSERT_NE(nullptr, body->get_value("input"));
  EXPECT_EQ(2, body->get_value("input")->element_count());

  ObString response(R"({"embeddings":[[7,8],[3,4]]})");
  ObJsonObject *object = nullptr;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, response, object));
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator, object, result));
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(2, result->element_count());
  ObIJsonBase *first = nullptr;
  ObIJsonBase *value = nullptr;
  ASSERT_EQ(OB_SUCCESS, result->get_array_element(0, first));
  ASSERT_EQ(OB_SUCCESS, first->get_array_element(0, value));
  EXPECT_EQ(7, value->get_int());
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncJsonUtils::validate_embedding(first, 3));
}
