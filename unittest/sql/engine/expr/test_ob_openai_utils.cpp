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
#include <limits>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

TEST(OpenAIEmbed, request_omits_default_and_forwards_explicit_dimension)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObString model("embedding-model");
  ObArray<ObString> input;
  ASSERT_EQ(OB_SUCCESS, input.push_back(ObString("one row")));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, input, nullptr, body));
  EXPECT_EQ(nullptr, body->get_value("dimensions"));

  ObJsonObject *config = nullptr;
  ObJsonInt *dimension = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator, config));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_int(allocator, 768, dimension));
  ASSERT_EQ(OB_SUCCESS, config->add("dimensions", dimension));
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, input, config, body));
  ASSERT_NE(nullptr, body->get_value("dimensions"));
  EXPECT_EQ(768, body->get_value("dimensions")->get_int());
}

TEST(OpenAIEmbed, parses_single_response_and_checks_expected_length)
{
  ObArenaAllocator allocator;
  ObString json(R"({"data":[{"index":0,"embedding":[1,0.25,-0.5,0]}]})");
  ObJsonObject *response = nullptr;
  ObIJsonBase *result = nullptr;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, response));
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator, response, result));
  ASSERT_EQ(1, result->element_count());
  auto *vector = static_cast<ObJsonArray *>(result)->get_value(0);
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::validate_embedding(vector, 0));
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::validate_embedding(vector, 4));
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncJsonUtils::validate_embedding(vector, 3));
}

TEST(OpenAIEmbed, rejects_bad_response_shape)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (const char *json : {R"({})", R"({"data":{}})", R"({"data":[null]})",
                           R"({"data":[{}]})", R"({"data":[{"embedding":"x"}]})"}) {
    ObString input(json);
    ObJsonObject *response = nullptr;
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, response));
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator, response, result));
    EXPECT_EQ(nullptr, result);
  }
}

TEST(OpenAIEmbed, rejects_invalid_elements)
{
  ObArenaAllocator allocator;
  for (const char *json : {R"({"v":[]})", R"({"v":[null]})", R"({"v":["1"]})",
                           R"({"v":[{}]})", R"({"v":[[]]})", R"({"v":[true]})"}) {
    ObString input(json);
    ObJsonObject *object = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, object));
    EXPECT_EQ(OB_INVALID_DATA, ObAIFuncJsonUtils::validate_embedding(object->get_value("v"), 0));
  }
  for (double nonfinite : {std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity()}) {
    ObJsonArray *array = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator, array));
    auto *element = OB_NEWx(ObJsonDouble, &allocator, nonfinite);
    ASSERT_NE(nullptr, element);
    ASSERT_EQ(OB_SUCCESS, array->append(element));
    EXPECT_EQ(OB_INVALID_DATA, ObAIFuncJsonUtils::validate_embedding(array, 1));
  }
}
