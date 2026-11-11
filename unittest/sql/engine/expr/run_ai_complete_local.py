#!/usr/bin/env python3
"""Exercise extracted completion request/response bodies with stub JSON nodes."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
SOURCE = "src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp"
BASELINE = "9b48d4bd8d42e7b60f71cfa6e13e3549b58ae062"

STUBS = r"""
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>
constexpr int OB_SUCCESS = 0, OB_INVALID_ARGUMENT = -1, OB_INVALID_DATA = -2;
#define OB_FAIL(x) ((ret = (x)) != OB_SUCCESS)
#define OB_SUCC(x) ((x) == OB_SUCCESS)
#define OB_ISNULL(x) (!(x))
#define OB_NOT_NULL(x) (!!(x))
#define UNUSED(x) (void)(x)
#define LOG_USER_ERROR(...) ((void)0)
using ObString = std::string;
struct ObIAllocator {};
struct ObJsonPath {
  ObJsonPath(ObString, ObIAllocator *) {}
  int parse_path() { return 0; }
  int path_node_cnt() { return 4; }
};
enum class ObJsonNodeType { J_NULL, J_INT, J_UINT, J_DOUBLE, J_DECIMAL, J_STRING, J_ARRAY, J_OBJECT,
                            J_BOOLEAN, J_OINT, J_OLONG, J_ODOUBLE, J_ODECIMAL, J_OFLOAT };
struct Decimal {
  long double value;
  int compare(int64_t rhs) const { return (value > rhs) - (value < rhs); }
};
struct ObJsonNode {
  ObJsonNodeType type;
  long double number = 0;
  ObString text;
  explicit ObJsonNode(ObJsonNodeType type_arg, long double value = 0): type(type_arg), number(value) {}
  ObJsonNodeType json_type() const { return type; }
  uint64_t get_data_length() const { return text.size(); }
  double get_double() const { return number; }
  float get_float() const { return number; }
  int64_t get_int() const { return number; }
  uint64_t get_uint() const { return number; }
  Decimal get_decimal_data() const { return {number}; }
  int seek(ObJsonPath &, int, bool, bool, std::vector<ObJsonNode *> &);
};
using ObIJsonBase = ObJsonNode;
using ObJsonString = ObJsonNode;
using ObJsonSeekResult = std::vector<ObJsonNode *>;
struct ObJsonObject : ObJsonNode {
  std::map<ObString, ObJsonNode *> values;
  ObJsonObject(): ObJsonNode(ObJsonNodeType::J_OBJECT) {}
  ObJsonNode *get_value(const ObString &key) {
    auto entry = values.find(key);
    return entry == values.end() ? nullptr : entry->second;
  }
  int add(const ObString &key, ObJsonNode *value) { values[key] = value; return 0; }
};
struct ObJsonArray : ObJsonNode {
  std::vector<ObJsonNode *> values;
  ObJsonArray(): ObJsonNode(ObJsonNodeType::J_ARRAY) {}
  ObJsonNode *get_value(uint64_t i) { return i < values.size() ? values[i] : nullptr; }
  uint64_t element_count() const { return values.size(); }
};
int ObJsonNode::seek(ObJsonPath &, int, bool, bool, ObJsonSeekResult &out) {
  auto *choices = static_cast<ObJsonObject *>(this)->get_value("choices");
  if (choices && choices->json_type() == ObJsonNodeType::J_ARRAY) {
    auto *choice = static_cast<ObJsonArray *>(choices)->get_value(0);
    if (choice && choice->json_type() == ObJsonNodeType::J_OBJECT) {
      auto *message = static_cast<ObJsonObject *>(choice)->get_value("message");
      if (message && message->json_type() == ObJsonNodeType::J_OBJECT) {
        auto *content = static_cast<ObJsonObject *>(message)->get_value("content");
        if (content) out.push_back(content);
      }
    }
  }
  return 0;
}
namespace common {
  using ::ObIAllocator; using ::ObString; using ::ObJsonObject; using ::ObIJsonBase;
}
struct ObAIFuncJsonUtils {
  static int get_json_object(ObIAllocator &, ObJsonObject *&out) {
    static ObJsonObject object;
    object.values.clear();
    out = &object;
    return 0;
  }
  static int get_json_string(ObIAllocator &, ObString &text, ObJsonString *&out) {
    static ObJsonString node(ObJsonNodeType::J_STRING);
    node.text = text;
    out = &node;
    return 0;
  }
  static int compact_json_object(ObIAllocator &, ObJsonObject *config, ObJsonObject *body) {
    if (config) for (auto &[key, value] : config->values) body->add(key, value);
    return 0;
  }
};
struct ObOpenAIUtils {
  struct ObOpenAIComplete {
    int get_body(ObIAllocator &, ObString &, ObString &, ObString &, ObJsonObject *, ObJsonObject *&);
    int parse_output(ObIAllocator &, ObJsonObject *, ObIJsonBase *&);
    int construct_messages_array(ObIAllocator &, ObString &, ObString &, ObJsonArray *&out) {
      static ObJsonArray array;
      out = &array;
      return 0;
    }
  };
};
"""

TESTS = r"""
int main() {
  ObIAllocator allocator;
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObString model("configured-model"), prompt, input("question");
  ObJsonObject config, response, choice, message;
  ObJsonArray choices;
  ObJsonNode number(ObJsonNodeType::J_INT, 16), zero(ObJsonNodeType::J_INT),
      two(ObJsonNodeType::J_INT, 2), decimal(ObJsonNodeType::J_DECIMAL, 0.2),
      fractional(ObJsonNodeType::J_DOUBLE, 1.5), too_high(ObJsonNodeType::J_DOUBLE, 2.1),
      nan(ObJsonNodeType::J_DOUBLE, std::numeric_limits<double>::quiet_NaN()),
      negative(ObJsonNodeType::J_INT, -1), null(ObJsonNodeType::J_NULL),
      overflowing(ObJsonNodeType::J_UINT, 9223372036854775808.0L),
      empty(ObJsonNodeType::J_STRING), text(ObJsonNodeType::J_STRING);
  text.text = "answer";
  int checks = 0, failures = 0;
  auto expect = [&](const char *name, bool ok) {
    ++checks;
    if (!ok) { ++failures; std::cout << "FAIL " << name << '\n'; }
  };
  auto request = [&](const char *name, ObJsonObject *options, int expected) {
    ObJsonObject *body = nullptr;
    const int rc = provider.get_body(allocator, model, prompt, input, options, body);
    bool ok = rc == expected && (body != nullptr) == (expected == OB_SUCCESS);
    if (ok && body) {
      ok = body->get_value("model")->text == model
           && body->get_value("temperature") == (options ? options->get_value("temperature") : nullptr)
           && body->get_value("max_tokens") == (options ? options->get_value("max_tokens") : nullptr);
    }
    expect(name, ok);
  };
  request("config omitted", nullptr, OB_SUCCESS);
  config.values = {{"temperature", &decimal}, {"max_tokens", &number}};
  request("numeric fields forwarded", &config, OB_SUCCESS);
  config.values["temperature"] = &zero;
  request("zero temperature", &config, OB_SUCCESS);
  config.values["temperature"] = &two;
  request("upper temperature", &config, OB_SUCCESS);
  config.values["temperature"] = &null;
  request("explicit JSON null", &config, OB_SUCCESS);
  for (auto *bad : {&too_high, &negative, &text, &nan}) {
    config.values["temperature"] = bad;
    request("invalid temperature", &config, OB_INVALID_ARGUMENT);
  }
  config.values.erase("temperature");
  for (auto *bad : {&zero, &negative, &fractional, &text, &overflowing}) {
    config.values["max_tokens"] = bad;
    request("invalid max_tokens", &config, OB_INVALID_ARGUMENT);
  }
  config.values = {{"model", &text}};
  request("reserved model key", &config, OB_INVALID_ARGUMENT);

  auto parse = [&](const char *name, int expected, ObJsonNode *value = nullptr) {
    ObIJsonBase *out = nullptr;
    const int rc = provider.parse_output(allocator, &response, out);
    expect(name, rc == expected && out == value);
  };
  parse("missing choices", OB_INVALID_DATA);
  response.values["choices"] = &choices;
  parse("empty choices", OB_INVALID_DATA);
  choices.values = {&choice};
  parse("missing message", OB_INVALID_DATA);
  choice.values["message"] = &message;
  parse("missing content", OB_INVALID_DATA);
  for (auto *bad : {&empty, &null, static_cast<ObJsonNode *>(&choice), &number}) {
    message.values["content"] = bad;
    parse("invalid content", OB_INVALID_DATA);
  }
  message.values["content"] = &text;
  parse("valid content", OB_SUCCESS, &text);
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures != 0;
}
"""


def section(text, start, end):
    offset = text.index(start)
    return text[offset:text.index(end, offset)]


with tempfile.TemporaryDirectory(prefix="ai_complete_local_") as tmp:
    for label in ("baseline", "working"):
        if label == "baseline":
            source = subprocess.check_output(["git", "show", f"{BASELINE}:{SOURCE}"], cwd=ROOT, text=True)
        else:
            source = (ROOT / SOURCE).read_text()
        start = "static int validate_openai_completion_config"
        body = "int ObOpenAIUtils::ObOpenAIComplete::get_body"
        code = section(source, start if start in source else body,
                       "int ObOpenAIUtils::ObOpenAIComplete::construct_messages_array")
        code += section(source, "int ObOpenAIUtils::ObOpenAIComplete::parse_output",
                        "int ObOpenAIUtils::ObOpenAIEmbed::get_header")
        cpp = Path(tmp) / f"{label}.cpp"
        binary = Path(tmp) / label
        cpp.write_text(STUBS + code + TESTS)
        subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-no-pie",
                        str(cpp), "-o", str(binary)], check=True)
        print(label, flush=True)
        result = subprocess.run([str(binary)])
        if label == "working":
            result.check_returncode()
