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
#include "share/ai_service/ob_ai_service_struct.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;
using oceanbase::share::ObAiModelEndpointInfo;

TEST(OllamaEndpoint, create_and_alter_with_optional_credentials)
{
  ObArenaAllocator allocator;
  ObAiModelEndpointInfo endpoint;
  ObString name("endpoint");
  ObString openai(R"({"ai_model_name":"logical","url":"http://localhost","provider":"OpenAI","access_key":"fixture"})");
  ObJsonObject *params = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, openai, params));
  ASSERT_EQ(OB_SUCCESS, endpoint.parse_from_json_base(allocator, name, *params));
  ObString alter(R"({"provider":"Ollama","access_key":null})");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, alter, params));
  ASSERT_EQ(OB_SUCCESS, endpoint.merge_delta_endpoint(allocator, *params));
  EXPECT_EQ(ObString("Ollama"), endpoint.get_provider());
  EXPECT_TRUE(endpoint.get_encrypted_access_key().empty());
  EXPECT_EQ(ObString("logical"), endpoint.get_ai_model_name());

  ObString omitted(R"({"ai_model_name":"logical","url":"http://localhost","provider":"OLLAMA"})");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, omitted, params));
  EXPECT_EQ(OB_SUCCESS, endpoint.parse_from_json_base(allocator, name, *params));
  EXPECT_TRUE(endpoint.get_encrypted_access_key().empty());
  ObString missing(R"({"ai_model_name":"logical","url":"http://localhost","provider":"openai"})");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, missing, params));
  EXPECT_EQ(OB_AI_FUNC_PARAM_EMPTY, endpoint.parse_from_json_base(allocator, name, *params));
}

TEST(OllamaProvider, dispatches_case_insensitively_and_sends_json_header)
{
  ObArenaAllocator allocator;
  ObString empty_key;
  for (const char *name : {"ollama", "Ollama", "OLLAMA"}) {
    ObAIFuncIComplete *complete = nullptr;
    ObAIFuncIEmbed *embed = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator, ObString(name), complete));
    ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator, ObString(name), embed));
    ObArray<ObString> complete_headers;
    ObArray<ObString> embed_headers;
    ASSERT_EQ(OB_SUCCESS, complete->get_header(allocator, empty_key, complete_headers));
    ASSERT_EQ(OB_SUCCESS, embed->get_header(allocator, empty_key, embed_headers));
    ASSERT_EQ(1, complete_headers.count());
    ASSERT_EQ(1, embed_headers.count());
    EXPECT_EQ(ObString("Content-Type: application/json"), complete_headers.at(0));
    EXPECT_EQ(complete_headers.at(0), embed_headers.at(0));
  }

  ObAIFuncIComplete *complete = nullptr;
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator, ObString("OpenAI"), complete));
  EXPECT_EQ(OB_INVALID_ARGUMENT, complete->get_header(allocator, empty_key, headers));
}

TEST(OllamaProvider, builds_generate_and_checks_native_response)
{
  ObArenaAllocator allocator;
  ObOllamaUtils::ObOllamaComplete provider;
  ObString model("model"), system("instructions"), prompt("question");
  ObString json(R"({"stream":true,"temperature":0.5,"max_tokens":16,"options":{"seed":3}})");
  ObJsonObject *config = nullptr;
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, config));
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, system, prompt, config, body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  ASSERT_NE(nullptr, body->get_value("prompt"));
  EXPECT_EQ(prompt.length(), body->get_value("prompt")->get_data_length());
  EXPECT_EQ(nullptr, body->get_value("max_tokens"));
  EXPECT_EQ(nullptr, body->get_value("temperature"));
  auto *options = static_cast<ObJsonObject *>(body->get_value("options"));
  ASSERT_NE(nullptr, options);
  ASSERT_NE(nullptr, options->get_value("num_predict"));
  EXPECT_EQ(16, options->get_value("num_predict")->get_int());
  ASSERT_NE(nullptr, options->get_value("seed"));
  EXPECT_EQ(3, options->get_value("seed")->get_int());
  double temperature = 0;
  ASSERT_NE(nullptr, options->get_value("temperature"));
  ASSERT_EQ(OB_SUCCESS, options->get_value("temperature")->to_double(temperature));
  EXPECT_DOUBLE_EQ(0.5, temperature);

  ObIJsonBase *result = nullptr;
  for (const char *bad : {R"({})", R"({"response":""})", R"({"response":null})", R"({"response":[]})"}) {
    ObString response(bad);
    ObJsonObject *object = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, response, object));
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator, object, result));
    EXPECT_EQ(nullptr, result);
  }
  ObString response(R"({"response":"answer","done":true})");
  ObJsonObject *object = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, response, object));
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator, object, result));
  ASSERT_NE(nullptr, result);
  EXPECT_EQ(6, result->get_data_length());
}

TEST(OllamaProvider, builds_embed_and_reuses_vector_validation)
{
  ObArenaAllocator allocator;
  ObOllamaUtils::ObOllamaEmbed provider;
  ObString model("model");
  ObArray<ObString> input;
  ASSERT_EQ(OB_SUCCESS, input.push_back(ObString("question")));
  ObString json(R"({"dimensions":2})");
  ObJsonObject *config = nullptr;
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, config));
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, input, config, body));
  ASSERT_NE(nullptr, body->get_value("input"));
  EXPECT_EQ(1, body->get_value("input")->element_count());
  ASSERT_NE(nullptr, body->get_value("dimensions"));
  EXPECT_EQ(2, body->get_value("dimensions")->get_int());
  ObJsonObject *response = nullptr;
  ObIJsonBase *result = nullptr;
  ObString good(R"({"embeddings":[[0.5,0.25]]})");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, good, response));
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator, response, result));
  ASSERT_EQ(1, result->element_count());
  ObIJsonBase *vector = nullptr;
  ASSERT_EQ(OB_SUCCESS, result->get_array_element(0, vector));
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::validate_embedding(vector, 2));
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncJsonUtils::validate_embedding(vector, 3));
  for (const char *json_text : {R"({})", R"({"embeddings":null})", R"({"embeddings":[]})"}) {
    ObString bad(json_text);
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, bad, response));
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator, response, result));
    EXPECT_EQ(nullptr, result);
  }
}
