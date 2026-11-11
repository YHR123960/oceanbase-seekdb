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
#include <string>
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

TEST(OpenAIComplete, preserves_long_prompt_and_explicit_config)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObString model("endpoint-model");
  ObString system;
  std::string long_prompt(128 * 1024, 'x');
  ObString prompt(long_prompt.size(), long_prompt.data());
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, system, prompt, nullptr, body));
  EXPECT_EQ(nullptr, body->get_value("temperature"));
  EXPECT_EQ(nullptr, body->get_value("max_tokens"));
  ObJsonNode *messages = body->get_value("messages");
  ASSERT_NE(nullptr, messages);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, messages->json_type());
  ASSERT_EQ(1, messages->element_count());
  auto *message = static_cast<ObJsonArray *>(messages)->get_value(0);
  ASSERT_NE(nullptr, message);
  ASSERT_EQ(ObJsonNodeType::J_OBJECT, message->json_type());
  auto *content = static_cast<ObJsonObject *>(message)->get_value("content");
  ASSERT_NE(nullptr, content);
  EXPECT_EQ(long_prompt, std::string(content->get_data(), content->get_data_length()));

  ObString json(R"({"temperature":0.2,"max_tokens":16})");
  ObJsonObject *config = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, config));
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, system, prompt, config, body));
  ASSERT_NE(nullptr, body->get_value("temperature"));
  ASSERT_NE(nullptr, body->get_value("max_tokens"));
  double temperature = 0;
  ASSERT_EQ(OB_SUCCESS, body->get_value("temperature")->to_double(temperature));
  EXPECT_DOUBLE_EQ(0.2, temperature);
  EXPECT_EQ(16, body->get_value("max_tokens")->get_int());
}

TEST(OpenAIComplete, rejects_invalid_configuration)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObString model("endpoint-model");
  ObString system;
  ObString prompt("question");
  for (const char *json : {R"({"temperature":"0.2"})", R"({"temperature":-0.1})",
                           R"({"temperature":2.1})", R"({"max_tokens":0})",
                           R"({"max_tokens":1.5})", R"({"max_tokens":"16"})",
                           R"({"max_tokens":9223372036854775808})",
                           R"({"messages":[]})", R"({"model":"override"})"}) {
    ObString input(json);
    ObJsonObject *config = nullptr;
    ObJsonObject *body = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, config));
    EXPECT_EQ(OB_INVALID_ARGUMENT, provider.get_body(allocator, model, system, prompt, config, body));
    EXPECT_EQ(nullptr, body);
  }
  ObString array("[]");
  ObJsonObject *config = nullptr;
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncJsonUtils::get_json_object_form_str(allocator, array, config));
  EXPECT_EQ(nullptr, config);
}

TEST(OpenAIComplete, rejects_invalid_responses_and_clears_output)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObIJsonBase *result = nullptr;
  ObString good(R"({"choices":[{"message":{"content":"answer"}}]})");
  ObJsonObject *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, good, response));
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator, response, result));
  ASSERT_NE(nullptr, result);
  EXPECT_EQ("answer", std::string(result->get_data(), result->get_data_length()));
  auto *previous = result;
  for (const char *json : {R"({})", R"({"choices":{}})", R"({"choices":[]})",
                           R"({"choices":[{}]})", R"({"choices":[{"message":[]}]})",
                           R"({"choices":[{"message":{}}]})",
                           R"({"choices":[{"message":{"content":null}}]})",
                           R"({"choices":[{"message":{"content":""}}]})",
                           R"({"choices":[{"message":{"content":10}}]})"}) {
    ObString input(json);
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, response));
    result = previous;
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator, response, result));
    EXPECT_EQ(nullptr, result);
  }
}
