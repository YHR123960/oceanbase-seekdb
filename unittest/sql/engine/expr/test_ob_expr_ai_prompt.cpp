// AI_PROMPT regressions. Uses the production JSON parser and prompt renderer.
#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_expr_ai_prompt.h"

using namespace oceanbase::common;

static int render(ObIAllocator &allocator, const char *json, ObString &result)
{
  ObJsonObject *object = nullptr;
  ObString input(json);
  int ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, object);
  return ret == OB_SUCCESS
      ? ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, object, result)
      : ret;
}

TEST(AIPrompt, repeated_adjacent_and_inserted_placeholders)
{
  ObArenaAllocator allocator;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(allocator,
      R"({"template":"{0}{1}{0}{0}","args":["a sufficiently long {1}","中文"]})", result));
  EXPECT_EQ(std::string("a sufficiently long {1}中文a sufficiently long {1}a sufficiently long {1}"),
      std::string(result.ptr(), result.length()));
}

TEST(AIPrompt, json_containers_and_nested_prompt)
{
  ObArenaAllocator allocator;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(allocator,
      R"({"template":"{0}|{1}|{2}|{3}","args":[[],{}, {"k":["中文",{}]}, {"template":"inner {0}","args":["value"]}]})", result));
  // Compare the container serialization with the same production JSON printer.
  ObJsonObject *object = nullptr;
  ObString input(R"({"k":["中文",{}]})");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, object));
  ObString printed;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator, object, printed));
  EXPECT_EQ(std::string("[]|{}|") + std::string(printed.ptr(), printed.length()) + "|inner value",
      std::string(result.ptr(), result.length()));
}

TEST(AIPrompt, invalid_indexes_and_literal_braces)
{
  ObArenaAllocator allocator;
  ObString result;
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(allocator,
      R"({"template":"{1}","args":["x"]})", result));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(allocator,
      R"({"template":"{999999999999999999999999999999}","args":["x"]})", result));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(allocator,
      R"({"template":"{0}","args":[]})", result));
  ASSERT_EQ(OB_SUCCESS, render(allocator,
      R"({"template":"{} {-1} {x} {0 \\{0}","args":["ok"]})", result));
  EXPECT_EQ(std::string("{} {-1} {x} {0 \\ok"), std::string(result.ptr(), result.length()));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(allocator,
      R"({"template":"","args":[]})", result));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(allocator,
      R"({"template":null,"args":[]})", result));
}

TEST(AIPrompt, long_and_empty_arguments)
{
  ObArenaAllocator allocator;
  ObJsonArray *args = nullptr;
  ObJsonObject *prompt = nullptr;
  ObJsonString *arg = nullptr;
  std::string long_text(1024 * 1024, 'a');
  ObString value(long_text.size(), long_text.data());
  ObString tmpl("{0}{0}{0}{0}");
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator, args));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator, value, arg));
  ASSERT_EQ(OB_SUCCESS, args->append(arg));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator, tmpl, args, prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  EXPECT_EQ(long_text + long_text + long_text + long_text, std::string(result.ptr(), result.length()));
  ASSERT_EQ(OB_SUCCESS, render(allocator, R"({"template":"{0}{0}","args":[""]})", result));
  EXPECT_EQ(0, result.length());
}

TEST(AIPrompt, sql_type_deduction)
{
  ObArenaAllocator allocator;
  oceanbase::sql::ObExprAIPrompt op(allocator);
  ObExprTypeCtx ctx;
  oceanbase::sql::ObExprResType result;
  oceanbase::sql::ObExprResType types[3];
  for (ObObjType lob_type : {ObTextType, ObLongTextType}) {
    types[0].set_type(lob_type);
    types[1].set_type(lob_type);
    types[2].set_json();
    ASSERT_EQ(OB_SUCCESS, op.calc_result_typeN(result, types, 3, ctx));
    EXPECT_EQ(ObJsonType, result.get_type());
    EXPECT_EQ(lob_type, types[0].get_calc_type());
    EXPECT_EQ(lob_type, types[1].get_calc_type());
    EXPECT_EQ(ObJsonType, types[2].get_calc_type());
    EXPECT_EQ(CS_TYPE_UTF8MB4_BIN, types[0].get_calc_collation_type());
    EXPECT_EQ(CS_TYPE_UTF8MB4_BIN, types[1].get_calc_collation_type());
  }
  types[0].set_type(ObIntType);
  EXPECT_EQ(OB_ERR_INVALID_TYPE_FOR_OP, op.calc_result_typeN(result, types, 1, ctx));
  types[0].set_type(ObVarcharType);
  types[1].set_type(ObNullType);
  EXPECT_EQ(OB_ERR_INVALID_TYPE_FOR_OP, op.calc_result_typeN(result, types, 2, ctx));
}

TEST(AIPrompt, malformed_prompt_is_rejected)
{
  ObArenaAllocator allocator;
  ObString result;
  EXPECT_EQ(OB_INVALID_ARGUMENT,
      ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, nullptr, result));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(allocator,
      R"({"template":"x","args":{}})", result));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(allocator,
      R"({"template":"x","args":[null]})", result));
}
