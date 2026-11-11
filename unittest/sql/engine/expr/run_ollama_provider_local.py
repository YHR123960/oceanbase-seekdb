#!/usr/bin/env python3
"""Extract the Endpoint validator and Ollama Provider bodies and run them with API stubs."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
BASE = "2415c659c7e376309d063b80f08b9b442a0fe95e"
UTILS = "src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp"
ENDPOINT = "src/share/ai_service/ob_ai_service_struct.cpp"

STUBS = r"""
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <strings.h>
#include <vector>
constexpr int OB_SUCCESS=0, OB_INVALID_ARGUMENT=-1, OB_INVALID_DATA=-2, OB_ERR_UNEXPECTED=-3,
    OB_AI_FUNC_PARAM_EMPTY=-4, OB_AI_FUNC_PARAM_VALUE_INVALID=-5;
#define OB_FAIL(x) ((ret=(x))!=0)
#define OB_SUCC(x) ((x)==0)
#define OB_ISNULL(x) (!(x))
#define OB_NOT_NULL(x) (!!(x))
#define ARRAYSIZEOF(x) (sizeof(x)/sizeof((x)[0]))
#define LOG_USER_ERROR(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define UNUSED(x) (void)(x)
struct ObString : std::string {
  using std::string::string;
  int case_compare(const char *other) const { return strcasecmp(c_str(), other); }
  int case_compare(const ObString &other) const { return case_compare(other.c_str()); }
};
template <typename T> struct ObArray : std::vector<T> {
  int push_back(T x) { std::vector<T>::push_back(x); return 0; }
};
enum class ObJsonNodeType { J_NULL, J_OBJECT, J_ARRAY, J_STRING, J_BOOLEAN,
                            J_INT, J_OINT, J_UINT, J_OLONG, J_DOUBLE, J_ODOUBLE,
                            J_DECIMAL, J_ODECIMAL, J_OFLOAT };
struct ObJsonPath {
  ObString path;
  ObJsonPath(ObString p, void *):path(p) {}
  int parse_path() { return 0; }
  int path_node_cnt() { return 1; }
};
struct Decimal { double n; int compare(int64_t v) const { return (n>v)-(n<v); } };
struct ObJsonNode {
  ObJsonNodeType type;
  ObString text;
  double number = 0;
  explicit ObJsonNode(ObJsonNodeType t):type(t) {}
  virtual ~ObJsonNode() = default;
  ObJsonNodeType json_type() const { return type; }
  uint64_t get_data_length() const { return text.size(); }
  virtual uint64_t element_count() const { return 0; }
  double get_double() const { return number; }
  float get_float() const { return number; }
  int64_t get_int() const { return number; }
  uint64_t get_uint() const { return number; }
  bool get_boolean() const { return number != 0; }
  Decimal get_decimal_data() const { return {number}; }
  int seek(ObJsonPath &, int, bool, bool, std::vector<ObJsonNode *> &);
};
using ObIJsonBase = ObJsonNode;
using ObJsonSeekResult = std::vector<ObJsonNode *>;
using ObJsonString = ObJsonNode;
using ObJsonBoolean = ObJsonNode;
struct ObJsonArray : ObJsonNode {
  ObJsonArray():ObJsonNode(ObJsonNodeType::J_ARRAY) {}
  std::vector<ObJsonNode *> items;
  uint64_t element_count() const { return items.size(); }
  ObJsonNode *get_value(uint64_t i) { return items.at(i); }
};
struct ObJsonObject : ObJsonNode {
  ObJsonObject():ObJsonNode(ObJsonNodeType::J_OBJECT) {}
  std::map<ObString, ObJsonNode *> items;
  ObJsonNode *get_value(const ObString &key) {
    auto i=items.find(key); return i==items.end() ? nullptr : i->second;
  }
  uint64_t element_count() const { return items.size(); }
  int get_key(uint64_t i, ObString &key) { key=std::next(items.begin(), i)->first; return 0; }
  ObJsonNode *get_value(uint64_t i) { return std::next(items.begin(), i)->second; }
  int add(const ObString &key, ObJsonNode *value) { items[key]=value; return 0; }
  int remove(const ObString &key) { items.erase(key); return 0; }
};
int ObJsonNode::seek(ObJsonPath &p, int, bool, bool, ObJsonSeekResult &out) {
  const ObString key(p.path.c_str()+2);
  if (auto *node=static_cast<ObJsonObject *>(this)->get_value(key)) out.push_back(node);
  return 0;
}
struct ObIAllocator {
  std::vector<std::unique_ptr<ObJsonNode>> nodes;
  template <typename T, typename... A> T *make(A... args) {
    auto item=std::make_unique<T>(args...);
    auto *ptr=item.get(); nodes.emplace_back(std::move(item)); return ptr;
  }
};
namespace common {
  using ::ObString; using ::ObIAllocator; using ::ObArray; using ::ObJsonObject; using ::ObIJsonBase;
}
struct ObAIFuncJsonUtils {
  static int get_json_object(ObIAllocator &a, ObJsonObject *&p) { p=a.make<ObJsonObject>(); return 0; }
  static int get_json_string(ObIAllocator &a, const ObString &s, ObJsonString *&p) {
    p=a.make<ObJsonNode>(ObJsonNodeType::J_STRING); p->text=s; return 0;
  }
  static int get_json_boolean(ObIAllocator &a, bool b, ObJsonBoolean *&p) {
    p=a.make<ObJsonNode>(ObJsonNodeType::J_BOOLEAN); p->number=b; return 0;
  }
  static int compact_json_object(ObIAllocator &, ObJsonObject *src, ObJsonObject *out) {
    if (src) for (auto &[k,v] : src->items) out->add(k,v);
    return 0;
  }
  static int transform_array_to_json_array(ObIAllocator &a, ObArray<ObString> &src, ObJsonArray *&out) {
    out=a.make<ObJsonArray>();
    for (const auto &s:src) { ObJsonString *p; get_json_string(a,s,p); out->items.push_back(p); }
    return 0;
  }
};
struct ObOllamaUtils {
  static int get_header(ObIAllocator &, ObArray<ObString> &);
  struct ObOllamaComplete {
    int get_header(ObIAllocator &, ObString &, ObArray<ObString> &);
    int get_body(ObIAllocator &, ObString &, ObString &, ObString &, ObJsonObject *, ObJsonObject *&);
    int set_config_json_format(ObIAllocator &, ObJsonObject *);
    int parse_output(ObIAllocator &, ObJsonObject *, ObIJsonBase *&);
  };
  struct ObOllamaEmbed {
    int get_header(ObIAllocator &, ObString &, ObArray<ObString> &);
    int get_body(ObIAllocator &, ObString &, ObArray<ObString> &, ObJsonObject *, ObJsonObject *&);
    int parse_output(ObIAllocator &, ObJsonObject *, ObIJsonBase *&);
  };
};
struct ObAiModelEndpointInfo {
  inline static ObString DEFAULT_SCOPE = "ALL";
  ObString name_="ep", scope_="all", ai_model_name_="logical", url_="url", access_key_, provider_,
      parameters_, request_transform_fn_, response_transform_fn_;
  int check_valid() const;
  static bool is_valid_provider(const ObString &);
};
"""

TESTS = r"""
int main() {
  int count=0, failures=0;
  auto check=[&](bool ok, const char *name) {
    ++count;
    if (!ok) { ++failures; std::cout<<"FAIL "<<name<<'\n'; }
  };
  ObAiModelEndpointInfo endpoint;
  for (auto name : {"ollama", "Ollama", "OLLAMA"}) {
    endpoint.provider_=name;
    check(endpoint.check_valid()==OB_SUCCESS, name);
  }
  endpoint.provider_="openai";
  check(endpoint.check_valid()==OB_AI_FUNC_PARAM_EMPTY, "OpenAI requires a key");
  endpoint.access_key_="test";
  check(endpoint.check_valid()==OB_SUCCESS, "OpenAI accepts a key");
  endpoint.provider_="invalid";
  check(endpoint.check_valid()==OB_AI_FUNC_PARAM_VALUE_INVALID, "unknown provider");
  ObIAllocator a;
  ObOllamaUtils::ObOllamaComplete complete;
  ObOllamaUtils::ObOllamaEmbed embed;
  ObArray<ObString> headers;
  ObString key, model("configured-model"), system("system"), input("input");
  check(complete.get_header(a,key,headers)==0 && headers.size()==1 &&
      headers[0]=="Content-Type: application/json", "JSON-only header");
  ObJsonObject *body=nullptr, config, response;
  ObJsonNode n(ObJsonNodeType::J_INT), text(ObJsonNodeType::J_STRING), nil(ObJsonNodeType::J_NULL);
  n.number=16;
  config.add("max_tokens", &n);
  int rc=complete.get_body(a,model,system,input,&config,body);
  check(rc==0 && body->get_value("stream") && !body->get_value("stream")->get_boolean(),
      "generation disables streaming");
  auto *options=body ? body->get_value("options") : nullptr;
  check(options && static_cast<ObJsonObject *>(options)->get_value("num_predict")==&n &&
      body->get_value("max_tokens")==nullptr, "max_tokens maps to options.num_predict");
  ObIJsonBase *out=nullptr;
  response.add("response",&text);
  check(complete.parse_output(a,&response,out)==OB_INVALID_DATA, "reject empty text");
  response.add("response",&nil);
  check(complete.parse_output(a,&response,out)==OB_INVALID_DATA, "reject null text");
  text.text="answer";
  response.add("response",&text);
  check(complete.parse_output(a,&response,out)==0 && out==&text, "native generation text");
  ObArray<ObString> inputs;
  inputs.push_back(input);
  ObJsonObject embed_config;
  embed_config.add("dimensions",&n);
  rc=embed.get_body(a,model,inputs,&embed_config,body);
  check(rc==0 && body->get_value("dimensions")==&n && body->get_value("input") &&
      body->get_value("input")->json_type()==ObJsonNodeType::J_ARRAY, "native embedding request");
  response.add("embeddings",&nil);
  check(embed.parse_output(a,&response,out)==OB_INVALID_DATA, "reject non-array embeddings");
  ObJsonArray vectors, vector;
  response.add("embeddings",&vectors);
  check(embed.parse_output(a,&response,out)==OB_INVALID_DATA, "reject empty embeddings");
  vectors.items.push_back(&vector);
  check(embed.parse_output(a,&response,out)==0 && out==&vectors, "native embedding envelope");
  std::cout<<count<<" checks, "<<failures<<" failures\n";
  return failures!=0;
}
"""


def read(path, label):
    if label == "working":
        return (ROOT / path).read_text()
    return subprocess.check_output(["git", "show", f"{BASE}:{path}"], cwd=ROOT, text=True)


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


with tempfile.TemporaryDirectory(prefix="ollama_local_") as directory:
    for label in ("baseline", "working"):
        utils, endpoint = (read(path, label) for path in (UTILS, ENDPOINT))
        code = section(endpoint, "const char *VALID_PROVIDERS[]", "#define EXTRACT_JSON_ELEM_STR")
        code += section(endpoint, "int ObAiModelEndpointInfo::check_valid",
                        "int ObAiModelEndpointInfo::merge_delta_endpoint")
        code += section(utils, "static int validate_openai_completion_config",
                        "int ObOpenAIUtils::ObOpenAIComplete::get_body")
        code += section(utils, "int ObOllamaUtils::get_header", "int ObDashscopeUtils::get_header")
        cpp, exe = (Path(directory) / name for name in (f"{label}.cpp", label))
        cpp.write_text(STUBS + code + TESTS)
        subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-no-pie",
                        str(cpp), "-o", str(exe)], check=True)
        print(label, flush=True)
        run = subprocess.run([str(exe)])
        if label == "working":
            run.check_returncode()
