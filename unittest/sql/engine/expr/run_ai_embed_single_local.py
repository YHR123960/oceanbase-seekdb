#!/usr/bin/env python3
"""Run extracted production embedding bodies against stub JSON and transport APIs."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
SOURCE = "src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp"
BASELINE = "f7f416856af687fc48320cd95f6b6343857ae96e"

STUBS = r"""
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

constexpr int OB_SUCCESS=0, OB_INVALID_ARGUMENT=-1, OB_INVALID_DATA=-2, OB_ERR_UNEXPECTED=-3;
#define OB_SUCC(x) ((x)==0)
#define OB_FAIL(x) ((ret=(x))!=0)
#define OB_ISNULL(x) (!(x))
#define OB_NOT_NULL(x) (!!(x))
#define LOG_USER_ERROR(...) ((void)0)
#define FORWARD_USER_ERROR(...) ((void)0)

struct ObIAllocator {};
using ObString = std::string;
template <class T> struct ObArray : std::vector<T> {
  using std::vector<T>::vector;
  int64_t count() const { return this->size(); }
  int push_back(T value) { std::vector<T>::push_back(value); return OB_SUCCESS; }
};
enum class ObJsonNodeType { J_ARRAY, J_OBJECT, J_INT, J_UINT, J_DECIMAL, J_DOUBLE,
                            J_STRING, J_NULL, J_BOOLEAN };
struct ObJsonNode {
  ObJsonNodeType type;
  double number = 0;
  explicit ObJsonNode(ObJsonNodeType t, double d = 0): type(t), number(d) {}
  virtual ~ObJsonNode() = default;
  ObJsonNodeType json_type() const { return type; }
  virtual uint64_t element_count() const { return 0; }
  virtual int get_array_element(uint64_t, ObJsonNode *&) const { return OB_INVALID_DATA; }
  double get_double() const { return number; }
  virtual int64_t get_int() const { return 0; }
  uint64_t get_uint() const { return 0; }
};
using ObIJsonBase = ObJsonNode;
struct ObJsonInt : ObJsonNode {
  int64_t value;
  explicit ObJsonInt(int64_t n): ObJsonNode(ObJsonNodeType::J_INT), value(n) {}
  int64_t get_int() const { return value; }
};
struct ObJsonArray : ObJsonNode {
  std::vector<ObJsonNode *> items;
  ObJsonArray(): ObJsonNode(ObJsonNodeType::J_ARRAY) {}
  uint64_t element_count() const override { return items.size(); }
  ObJsonNode *get_value(uint64_t i) const { return items.at(i); }
  int get_array_element(uint64_t i, ObJsonNode *&out) const override { out = get_value(i); return 0; }
  int append(ObJsonNode *n) { items.push_back(n); return 0; }
};
struct ObJsonObject : ObJsonNode {
  std::map<std::string, ObJsonNode *> entries;
  ObJsonObject(): ObJsonNode(ObJsonNodeType::J_OBJECT) {}
  ObJsonNode *get_value(const char *k) {
    auto found = entries.find(k);
    return found == entries.end() ? nullptr : found->second;
  }
};
namespace common {
  using ::ObIAllocator; using ::ObJsonObject; using ::ObIJsonBase;
}
struct ObAIFuncJsonUtils {
  static int get_json_array(ObIAllocator &, ObJsonArray *&out) {
    static ObJsonArray array;
    array.items.clear();
    out = &array;
    return 0;
  }
  static int validate_embedding(const ObIJsonBase *, int64_t);
  static int print_json_to_str(ObIAllocator &, ObIJsonBase *, ObString &s) {
    s = "serialized vector";
    return 0;
  }
};
struct ObAIFuncIEmbed {
  int get_header(ObIAllocator &, ObString &, ObArray<ObString> &) { return 0; }
  int get_body(ObIAllocator &, ObString &, ObArray<ObString> &, ObJsonObject *, ObJsonObject *&) { return 0; }
  virtual int parse_output(ObIAllocator &, ObJsonObject *, ObIJsonBase *&) = 0;
};
struct ObOpenAIUtils {
  struct ObOpenAIEmbed : ObAIFuncIEmbed {
    int parse_output(ObIAllocator &, ObJsonObject *, ObIJsonBase *&) override;
  };
};
struct ObAIFuncUtils {
  static int get_embed_provider(ObIAllocator &, ObString, ObAIFuncIEmbed *&out) {
    static ObOpenAIUtils::ObOpenAIEmbed provider;
    out = &provider;
    return 0;
  }
};
struct Endpoint {
  ObString get_provider() { return "openai"; }
  ObString get_url() { return "mock"; }
  int get_unencrypted_access_key(ObIAllocator &, ObString &) { return 0; }
};
ObJsonObject *mock_response;
int network_error;
struct ObAIFuncClient {
  int send_post(ObIAllocator &, ObString, ObArray<ObString> &, ObJsonObject *, ObJsonObject *&out) {
    out = mock_response;
    return network_error;
  }
};
struct ObAIFuncModel {
  ObIAllocator *allocator_;
  Endpoint endpoint_info_;
  bool is_dense_embedding_type() { return true; }
  ObString get_request_model_name() { return "test"; }
  int call_dense_embedding_vector_v2(ObArray<ObString> &, ObJsonObject *, ObArray<ObString> &);
};
"""

TESTS = r"""
int main() {
  ObIAllocator allocator;
  ObAIFuncModel model{&allocator, {}};
  ObArray<ObString> content{"input"};
  ObJsonArray vector, data;
  ObJsonObject item, response, config;
  ObJsonInt four(4), three(3), zero(0);
  ObJsonNode real(ObJsonNodeType::J_DOUBLE, 0.25), null(ObJsonNodeType::J_NULL),
      str(ObJsonNodeType::J_STRING), boolean(ObJsonNodeType::J_BOOLEAN),
      nan(ObJsonNodeType::J_DOUBLE, std::numeric_limits<double>::quiet_NaN()),
      infinity(ObJsonNodeType::J_DOUBLE, std::numeric_limits<double>::infinity());
  item.entries["embedding"] = &vector;
  data.items = {&item};
  response.entries["data"] = &data;
  mock_response = &response;
  int count = 0, failures = 0;
  auto check = [&](const char *name, int expected, ObJsonObject *options = nullptr) {
    ObArray<ObString> results;
    const int actual = model.call_dense_embedding_vector_v2(content, options, results);
    ++count;
    if (actual != expected || results.count() != (expected == OB_SUCCESS ? 1 : 0)) {
      ++failures;
      std::cout << "FAIL " << name << " (returned " << actual << ")\n";
    }
  };
  vector.items = {&real, &real, &real, &real};
  check("provider default", OB_SUCCESS);
  config.entries["dimensions"] = &four;
  check("explicit dimension", OB_SUCCESS, &config);
  config.entries["dimensions"] = &three;
  check("mismatched dimension", OB_INVALID_ARGUMENT, &config);
  config.entries["dimensions"] = &zero;
  check("zero dimension", OB_INVALID_ARGUMENT, &config);
  config.entries["dimensions"] = &str;
  check("non-integer config", OB_INVALID_ARGUMENT, &config);
  vector.items.clear();
  check("empty vector", OB_INVALID_DATA);
  for (auto *bad : std::vector<ObJsonNode *>{&null, &str, &boolean, &item, &data, &nan, &infinity}) {
    vector.items = {bad};
    check("illegal vector element", OB_INVALID_DATA);
  }
  item.entries["embedding"] = &str;
  check("non-array embedding", OB_INVALID_DATA);
  item.entries.clear();
  check("missing embedding", OB_INVALID_DATA);
  network_error = -99;
  check("network error propagated", -99);
  std::cout << count << " checks, " << failures << " failures\n";
  return failures != 0;
}
"""


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


with tempfile.TemporaryDirectory(prefix="ai_embed_local_") as tmp:
    for label in ("baseline", "working"):
        if label == "baseline":
            source = subprocess.check_output(
                ["git", "show", f"{BASELINE}:{SOURCE}"], cwd=ROOT, text=True
            )
        else:
            source = (ROOT / SOURCE).read_text()
        code = section(source, "int ObOpenAIUtils::ObOpenAIEmbed::parse_output", "int ObOllamaUtils::get_header")
        start = "int ObAIFuncJsonUtils::validate_embedding"
        if start in source:
            code += section(source, start, "int ObAIFuncModel::call_dense_embedding_vector_v2")
        code += section(source, "int ObAIFuncModel::call_dense_embedding_vector_v2", "int ObAIFuncModel::call_rerank")
        # Stub strings use std::string; retain the baseline diagnostic's pointer semantics.
        code = code.replace("response_str.ptr()", "response_str.c_str()")
        cpp = Path(tmp) / f"{label}.cpp"
        binary = Path(tmp) / label
        cpp.write_text(STUBS + code + TESTS)
        subprocess.run(
            ["g++", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-no-pie",
             str(cpp), "-o", str(binary)], check=True
        )
        print(label, flush=True)
        result = subprocess.run([str(binary)], env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})
        if label == "working":
            result.check_returncode()
