#!/usr/bin/env python3
"""Compile extracted rerank result functions with stub JSON/allocator APIs."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
SOURCE = "src/sql/engine/expr/ob_expr_ai/ob_expr_ai_rerank.cpp"
BASE = "bf50166958fff5fe34d7ac2bfaf88bbff2eba4d1"

STUBS = r"""
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

constexpr int OB_SUCCESS=0, OB_INVALID_ARGUMENT=-1, OB_INVALID_DATA=-2, OB_ERR_UNEXPECTED=-3,
    OB_ALLOCATE_MEMORY_FAILED=-4;
#define INIT_SUCC(ret) int ret=0
#define OB_SUCC(x) ((x)==0)
#define OB_FAIL(x) ((ret=(x))!=0)
#define OB_ISNULL(x) (!(x))
#define OB_NOT_NULL(x) (!!(x))
#define LOG_USER_ERROR(...) ((void)0)
struct ObString : std::string {
  using std::string::string;
  ObString(size_t length, const char *data):std::string(data,length) {}
};
enum class ObJsonNodeType { J_OBJECT, J_ARRAY, J_STRING, J_INT, J_UINT, J_DECIMAL, J_DOUBLE, J_NULL };
struct ObIAllocator;
struct ObJsonNode {
  ObJsonNodeType type;
  std::string text;
  explicit ObJsonNode(ObJsonNodeType t):type(t) {}
  virtual ~ObJsonNode()=default;
  ObJsonNodeType json_type() const { return type; }
  uint64_t get_data_length() const { return text.size(); }
  const char *get_data() const { return text.data(); }

  virtual int64_t get_int() const { return 0; }
  virtual uint64_t get_uint() const { return 0; }
  virtual int to_double(double &) const { return OB_INVALID_DATA; }
  virtual ObJsonNode *clone(ObIAllocator *, bool) const;
};
struct ObIAllocator {
  std::vector<std::unique_ptr<ObJsonNode>> objects;
  template <class T, class... A> T *make(A... args) {
    auto node=std::make_unique<T>(args...);
    auto *ptr=node.get(); objects.emplace_back(std::move(node)); return ptr;
  }
};
ObJsonNode *ObJsonNode::clone(ObIAllocator *a, bool) const { return a->make<ObJsonNode>(*this); }
using ObJsonString=ObJsonNode;
struct ObJsonInt : ObJsonNode {
  int64_t n;
  explicit ObJsonInt(int64_t value):ObJsonNode(ObJsonNodeType::J_INT),n(value) {}
  int64_t value() const { return n; }
  int64_t get_int() const override { return n; }
  int to_double(double &out) const override { out=n; return 0; }
};
struct ObJsonDouble : ObJsonNode {
  double n;
  explicit ObJsonDouble(double value):ObJsonNode(ObJsonNodeType::J_DOUBLE),n(value) {}
  int to_double(double &out) const override { out=n; return 0; }
};
struct ObJsonObject : ObJsonNode {
  std::map<std::string,ObJsonNode *> fields;
  ObJsonObject():ObJsonNode(ObJsonNodeType::J_OBJECT) {}
  int add(const char *key, ObJsonNode *value) { fields[key]=value; return 0; }
  ObJsonNode *get_value(const std::string &key) {
    auto i=fields.find(key); return i==fields.end()?nullptr:i->second;
  }
  ObJsonNode *clone(ObIAllocator *a, bool) const override { return a->make<ObJsonObject>(*this); }
};
struct ObJsonArray : ObJsonNode {
  std::vector<ObJsonNode *> items;
  ObJsonArray():ObJsonNode(ObJsonNodeType::J_ARRAY) {}
  uint64_t element_count() const { return items.size(); }
  ObJsonNode *get_value(uint64_t i) { return i<items.size()?items[i]:nullptr; }
  int append(ObJsonNode *value) { items.push_back(value); return 0; }
};
template <class T> struct ObArray : std::vector<T> {
  int64_t count() const { return this->size(); }
  T &at(int64_t i) { return std::vector<T>::at(i); }
  int push_back(const T &item) { std::vector<T>::push_back(item); return 0; }
};
struct ObAIFuncJsonUtils {
  static int get_json_array(ObIAllocator &a, ObJsonArray *&out) { out=a.make<ObJsonArray>(); return 0; }
  static int get_json_string(ObIAllocator &a, ObString &s, ObJsonString *&out) {
    out=a.make<ObJsonString>(ObJsonNodeType::J_STRING); out->text=s; return 0;
  }
  static int get_json_int(ObIAllocator &a, int64_t n, ObJsonInt *&out) {
    out=a.make<ObJsonInt>(n); return 0;
  }
};
struct ObExprAIRerank {
  static constexpr const char *INDEX_KEY="index", *SCORE_KEY="relevance_score", *MODEL_SCORE_KEY="_model_score";
  static constexpr int64_t RERANK_BATCH_SIZE=20;
  struct RerankHit { uint64_t index_; double score_; int64_t sequence_; ObJsonObject *response_; };
  static int sort_document_array_by_model_result(ObIAllocator &, ObJsonArray *, ObJsonArray *, ObJsonArray *&);
  static int prepare_documents(ObIAllocator &, ObJsonArray *, const ObString *, ObJsonArray *&);
  static int collect_batch_hits(ObJsonArray *, uint64_t, uint64_t, ObArray<RerankHit> &);
  static int build_rerank_result(ObIAllocator &, ObJsonArray *, bool, uint64_t,
                                 ObArray<RerankHit> &, ObJsonArray *&);
};
"""

TESTS = r"""
int main() {
  ObIAllocator a;
  ObJsonArray docs, batch, out_of_order;
  for (auto id : {"first", "second", "third"}) {
    auto *object=a.make<ObJsonObject>();
    auto *value=a.make<ObJsonNode>(ObJsonNodeType::J_STRING);
    value->text=id;
    object->add("id",value);
    object->add("metadata",value);
    docs.append(object);
  }
  auto row=[&](int64_t index, double score) {
    auto *obj=a.make<ObJsonObject>();
    obj->add("index",a.make<ObJsonInt>(index));
    obj->add("relevance_score",a.make<ObJsonDouble>(score));
    return obj;
  };
  int checks=0, failures=0;
  auto check=[&](bool ok, const char *name) {
    ++checks;
    if (!ok) { ++failures; std::cout << "FAIL " << name << '\n'; }
  };
  auto run=[&](ObJsonArray &rows, ObJsonArray *&out) {
#ifdef BASELINE
    return ObExprAIRerank::sort_document_array_by_model_result(a,&docs,&rows,out);
#else
    ObArray<ObExprAIRerank::RerankHit> hits;
    int rc=ObExprAIRerank::collect_batch_hits(&rows,0,3,hits);
    return rc ? rc : ObExprAIRerank::build_rerank_result(a,&docs,true,3,hits,out);
#endif
  };
  auto id_is=[](ObJsonArray *out, int i, const char *id) {
    auto *node=out ? out->get_value(i) : nullptr;
    return node && static_cast<ObJsonObject *>(node)->get_value("id")->text==id;
  };
  ObJsonArray *out=nullptr;
  out_of_order.items={row(2,0.1),row(1,0.8),row(0,0.8)};
  int rc=run(out_of_order,out);
  check(rc==0 && id_is(out,0,"second") && id_is(out,1,"first") && id_is(out,2,"third"),
        "stable score order and ID mapping");
  check(rc==0 && static_cast<ObJsonObject *>(out->get_value(0))->get_value("metadata")!=nullptr,
        "preserve source properties");
  batch.items={row(1,0.8),row(1,0.2),row(2,0.1)};
  check(run(batch,out)==OB_INVALID_DATA,"duplicate index");
  batch.items={row(3,0.8),row(1,0.2),row(2,0.1)};
  check(run(batch,out)==OB_INVALID_DATA,"out of range index");
  batch.items={row(0,0.8),row(1,0.2)};
  check(run(batch,out)==OB_INVALID_DATA,"incomplete response");
  batch.items={row(0,0.8),row(1,std::numeric_limits<double>::infinity()),row(2,0.1)};
  check(run(batch,out)==OB_INVALID_DATA,"non-finite score");
  batch.items={row(0,0.8),row(1,0.2),a.make<ObJsonObject>()};
  check(run(batch,out)==OB_INVALID_DATA,"missing index and score");
#ifndef BASELINE
  ObJsonArray *texts=nullptr;
  ObString key("text");
  auto *plain=a.make<ObJsonString>(ObJsonNodeType::J_STRING);
  plain->text="query";
  ObJsonArray strings;
  strings.items={plain};
  check(ObExprAIRerank::prepare_documents(a,&strings,nullptr,texts)==0 &&
      texts->get_value(0)->text=="query","string documents");
  check(ObExprAIRerank::prepare_documents(a,&docs,&key,texts)==OB_INVALID_ARGUMENT,"missing text");
  for (auto *doc : docs.items) static_cast<ObJsonObject *>(doc)->add("text",plain);
  check(ObExprAIRerank::prepare_documents(a,&docs,&key,texts)==0 &&
      texts->element_count()==3 && texts->get_value(1)->text=="query","extract object text");
  strings.items.push_back(docs.items[0]);
  check(ObExprAIRerank::prepare_documents(a,&strings,nullptr,texts)==OB_INVALID_ARGUMENT,"mixed array");
  plain->text.clear();
  check(ObExprAIRerank::prepare_documents(a,&docs,&key,texts)==OB_INVALID_ARGUMENT,"empty text");
  ObJsonArray one,two;
  one.items={row(0,0.1),row(1,0.5)};
  two.items={row(0,0.9)};
  ObArray<ObExprAIRerank::RerankHit> hits;
  rc=ObExprAIRerank::collect_batch_hits(&one,0,2,hits);
  if (!rc) rc=ObExprAIRerank::collect_batch_hits(&two,2,1,hits);
  if (!rc) rc=ObExprAIRerank::build_rerank_result(a,&docs,true,1,hits,out);
  check(rc==0 && out->element_count()==1 && id_is(out,0,"third"),"cross-batch index and global top_n");
  if (!rc) rc=ObExprAIRerank::build_rerank_result(a,&docs,false,2,hits,out);
  check(rc==0 && out->element_count()==2 &&
      static_cast<ObJsonObject *>(out->get_value(0))->get_value("index")->get_int()==2,
      "string result retains provider shape and global index");
  auto *integer_score=row(0,0.1);
  integer_score->add("relevance_score",a.make<ObJsonInt>(1));
  batch.items={integer_score,row(1,0.8),row(2,0.9)};
  check(run(batch,out)==0 && id_is(out,0,"first"),"integer score");
  batch.items={row(-1,0.1),row(1,0.8),row(2,0.9)};
  check(run(batch,out)==OB_INVALID_DATA,"negative index");
#endif
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures!=0;
}
"""


def section(source, start, end):
    pos = source.index(start)
    return source[pos:source.index(end, pos)]


with tempfile.TemporaryDirectory(prefix="ai_rerank_local_") as directory:
    for label in ("baseline", "working"):
        source = (subprocess.check_output(["git", "show", f"{BASE}:{SOURCE}"], cwd=ROOT, text=True)
                  if label == "baseline" else (ROOT / SOURCE).read_text())
        legacy = "int ObExprAIRerank::collect_batch_hits" not in source
        if legacy:
            code = section(source, "int ObExprAIRerank::sort_document_array_by_model_result",
                           "int ObExprAIRerank::construct_batch_document_array")
        else:
            code = section(source, "int ObExprAIRerank::prepare_documents", "int ObExprAIRerank::cg_expr")
        cpp, exe = (Path(directory) / name for name in (f"{label}.cpp", label))
        cpp.write_text(STUBS + code + TESTS)
        subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-no-pie",
                        *(["-DBASELINE"] if legacy else []), str(cpp), "-o", str(exe)], check=True)
        print(label, flush=True)
        run = subprocess.run([str(exe)])
        if label == "working":
            run.check_returncode()
