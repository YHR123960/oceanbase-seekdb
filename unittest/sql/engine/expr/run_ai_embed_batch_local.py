#!/usr/bin/env python3
"""Test extracted batch callback with real loopback HTTP and protocol bodies with API stubs."""
import ast
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import subprocess
import tempfile
from threading import Thread

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
AI = "src/sql/engine/expr/ob_expr_ai"
BASE = "5847b40938e5533cc356662f4df856557f7817a3"

SQL_STUBS = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
constexpr int OB_SUCCESS=0, OB_INVALID_ARGUMENT=-1, OB_ERR_UNEXPECTED=-2, OB_ALLOCATE_MEMORY_FAILED=-3;
#define OB_FAIL(x) ((ret=(x))!=0)
#define OB_SUCC(x) ((x)==0)
#define OB_ISNULL(x) (!(x))
#define OB_NOT_NULL(x) (!!(x))
#define LOG_USER_ERROR(...) ((void)0)
#define MEMCPY memcpy
constexpr int N_AI_EMBED=0;
struct ObIAllocator {};
struct MultimodeAlloctor : ObIAllocator { explicit MultimodeAlloctor(ObIAllocator &) {} };
namespace lib {
struct ObMemAttr { explicit ObMemAttr(int) {} };
struct ObMallocHookAttrGuard { explicit ObMallocHookAttrGuard(ObMemAttr) {} };
}
struct ObString : std::string {
  using std::string::string;
  ObString(size_t n,const char *p):std::string(p,n) {}
  const char *ptr() const { return data(); }
};
template<class T> struct ObArray : std::vector<T> {
  int64_t count() const { return this->size(); }
  int push_back(T v) { std::vector<T>::push_back(v); return 0; }
};
enum ObObjType { ObNullType, ObIntType, ObUInt64Type };
bool ob_is_uint_tc(ObObjType t) { return t==ObUInt64Type; }
struct ObDatum {
  bool null_=true;
  ObString text;
  int64_t n=0;
  bool is_null() const { return null_; }
  void set_null() { null_=true; }
  void set_string(ObString s) { null_=false; text=s; }
  ObString get_string() const { return text; }
  int64_t get_int() const { return n; }
  uint64_t get_uint() const { return n; }
};
struct ObEvalCtx {
  struct TempAllocGuard {
    explicit TempAllocGuard(ObEvalCtx &) {}
    ObIAllocator allocator;
    ObIAllocator &get_allocator() { return allocator; }
  };
};
struct ObBitVector {
  bool bits[64]={};
  bool at(int64_t row) const { return bits[row]; }
  void set(int64_t row) { bits[row]=true; }
};
struct ObExpr {
  ObExpr *args_[3]={};
  int arg_cnt_=0;
  struct { ObObjType type_=ObIntType; } datum_meta_;
  mutable ObBitVector evaluated;
  mutable ObDatum data[64];
  mutable std::string buffers[64];
  ObBitVector &get_evaluated_flags(ObEvalCtx &) const { return evaluated; }
  int eval_batch(ObEvalCtx &,const ObBitVector &,int64_t) const { return 0; }
  ObDatum &locate_expr_datum(ObEvalCtx &,int64_t row) const { return data[row]; }
  char *get_str_res_mem(ObEvalCtx &,size_t len,int64_t row) const {
    buffers[row].resize(len); return buffers[row].data();
  }
};
struct ObExprAIEmbed {
  static constexpr int MODEL_IDX=0, CONTENT_IDX=1, DIM_IDX=2;
  static int eval_ai_embed_batch(const ObExpr &,ObEvalCtx &,const ObBitVector &,int64_t);
};
int port;
int requests=0;
// Replace only the SQL-to-provider boundary. Each invocation sends a real HTTP POST.
static int request_embeddings(ObIAllocator &,const ObString &model,int64_t dimension,
                              ObArray<ObString> &inputs,ObArray<ObString> &results)
{
  std::string json="{\"model\":\""+model+"\",\"dimensions\":"+std::to_string(dimension)+",\"input\":[";
  for (size_t i=0; i<inputs.size(); ++i) {
    if (i) json+=",";
    json+="\""+inputs[i]+"\"";
  }
  json+="]}";
  const std::string command="curl --silent --fail --max-time 5 --noproxy '*' "
      "-H 'Content-Type: application/json' --data-binary '"+json+
      "' http://127.0.0.1:"+std::to_string(port)+"/embeddings";
  FILE *pipe=popen(command.c_str(),"r");
  assert(pipe);
  ++requests;
  char line[64];
  while (fgets(line,sizeof(line),pipe)) {
    results.push_back(ObString(strcspn(line,"\n"),line));
  }
  return pclose(pipe)==0?OB_SUCCESS:OB_ERR_UNEXPECTED;
}
"""

SQL_TESTS = r"""
int main(int argc,char **argv) {
  assert(argc==2);
  port=std::stoi(argv[1]);
  ObEvalCtx ctx;
  ObExpr model,text,dim,result;
  ObBitVector skip;
  result.args_[0]=&model;
  result.args_[1]=&text;
  result.args_[2]=&dim;
  result.arg_cnt_=3;
  for (int i=0; i<45; ++i) {
    model.data[i].set_string("A");
    dim.data[i].null_=false;
    dim.data[i].n=2;
    text.data[i].set_string(ObString(std::to_string(i).c_str()));
  }
  for (int i : {0,8,9,31,44}) text.data[i].set_null();
  skip.set(7);
  skip.set(11);
  assert(ObExprAIEmbed::eval_ai_embed_batch(result,ctx,skip,45)==0);
  assert(requests==2);
  for (int i=0; i<45; ++i) {
    if (skip.at(i)) {
      assert(!result.evaluated.at(i));
    } else if (text.data[i].is_null()) {
      assert(result.data[i].is_null() && result.evaluated.at(i));
    } else {
      assert(result.data[i].get_string()=="["+std::to_string(i)+",2]");
    }
  }
  assert(ObExprAIEmbed::eval_ai_embed_batch(result,ctx,skip,45)==0 && requests==2);

  skip={};
  result.evaluated={};
  text.data[0].set_string("0");
  model.data[1].set_string("B");
  dim.data[3].n=3;
  assert(ObExprAIEmbed::eval_ai_embed_batch(result,ctx,skip,4)==0 && requests==5);
  assert(result.data[1].get_string()=="[1,2]" && result.data[3].get_string()=="[3,3,3]");

  result.evaluated={};
  text.data[0].set_null();
  text.data[1].set_null();
  assert(ObExprAIEmbed::eval_ai_embed_batch(result,ctx,skip,2)==0 && requests==5);
  assert(result.data[0].is_null() && result.data[1].is_null());

  result.evaluated={};
  text.data[0].set_string("0");
  assert(ObExprAIEmbed::eval_ai_embed_batch(result,ctx,skip,1)==0 && requests==6);
  assert(result.data[0].get_string()=="[0,2]");

  result.evaluated={};
  text.data[0].set_string("");
  assert(ObExprAIEmbed::eval_ai_embed_batch(result,ctx,skip,1)==OB_INVALID_ARGUMENT && requests==6);
  text.data[0].set_string("0");
  dim.data[0].n=0;
  assert(ObExprAIEmbed::eval_ai_embed_batch(result,ctx,skip,1)==OB_INVALID_ARGUMENT && requests==6);
  dim.data[0].n=2;
  text.data[0].set_string("fail");
  assert(ObExprAIEmbed::eval_ai_embed_batch(result,ctx,skip,1)==OB_ERR_UNEXPECTED && requests==7);
  assert(!result.evaluated.at(0));
  std::cout<<"Batch callback checks passed: NULLs, skip flags, rescans, grouping, one row and HTTP error\n";
}
"""

PROTOCOL_TESTS = r"""
int main() {
  ObIAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed openai;
  ObOllamaUtils::ObOllamaEmbed ollama;
  ObAIFuncModel model{&allocator, {}};
  ObJsonArray v0,v1,data,ollama_data;
  ObJsonNode x(ObJsonNodeType::J_DOUBLE,1),y(ObJsonNodeType::J_DOUBLE,2),
             bad(ObJsonNodeType::J_STRING);
  ObJsonInt i0(0),i1(1),i2(2),negative(-1),dim(2);
  v0.items={&x,&x}; v1.items={&y,&y};
  ObJsonObject first,second,response,config;
  first.entries={{"embedding",&v0},{"index",&i0}};
  second.entries={{"embedding",&v1},{"index",&i1}};
  data.items={&second,&first};
  response.entries["data"]=&data;
  mock_response=&response;
  int checks=0, failures=0;
  auto expect=[&](const char *name,bool ok) {
    ++checks;
    if(!ok) { ++failures; std::cout<<"FAIL "<<name<<'\n'; }
  };
  ObIJsonBase *out=nullptr;
  int rc=openai.parse_output(allocator,&response,out);
  expect("restore OpenAI index order",rc==0 &&
      static_cast<ObJsonArray *>(out)->get_value(0)==&v0);
  for(auto *index : {&i0,&i2,&negative}) {
    second.entries["index"]=index;
    expect("reject duplicate or invalid index",openai.parse_output(allocator,&response,out)==OB_INVALID_DATA);
  }
  second.entries.erase("index");
  expect("reject missing index",openai.parse_output(allocator,&response,out)==OB_INVALID_DATA);
  second.entries["index"]=&i1;
  ObArray<ObString> inputs{"a","b"};
  auto call=[&](ObJsonObject *options=nullptr) {
    ObArray<ObString> results;
    return model.call_dense_embedding_vector_v2(inputs,options,results);
  };
  expect("two valid vectors",call()==0);
  data.items={&first};
  expect("too few results",call()==OB_ERR_UNEXPECTED);
  ObJsonObject third;
  third.entries={{"embedding",&v0},{"index",&i2}};
  data.items={&second,&first,&third};
  expect("too many results",call()==OB_ERR_UNEXPECTED);
  data.items={&second,&first};
  v1.items={&y};
  config.entries["dimensions"]=&dim;
  expect("explicit dimension mismatch",call(&config)==OB_INVALID_ARGUMENT);
  expect("default dimension mismatch",call()==OB_INVALID_ARGUMENT);
  v1.items={&y,&bad};
  expect("invalid vector element",call()==OB_INVALID_DATA);
  first.entries.erase("index");
  data.items={&first};
  expect("single result without index remains compatible",openai.parse_output(allocator,&response,out)==0);
  ollama_data.items={&v1,&v0};
  response.entries["embeddings"]=&ollama_data;
  expect("Ollama preserves positional order",ollama.parse_output(allocator,&response,out)==0 &&
      static_cast<ObJsonArray *>(out)->get_value(0)==&v1);
  std::cout<<checks<<" checks, "<<failures<<" failures\n";
  return failures!=0;
}
"""


def section(source, start, end):
    offset = source.index(start)
    return source[offset:source.index(end, offset)]


def build(tmp, name, code):
    source, binary = (tmp / item for item in (name + ".cpp", name))
    source.write_text(code)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-no-pie",
                    str(source), "-o", str(binary)], check=True)
    return binary


def protocol_checks(tmp):
    # Share the JSON and model test doubles with the single-row regression.
    tree = ast.parse((HERE / "run_ai_embed_single_local.py").read_text())
    stubs = next(ast.literal_eval(node.value) for node in tree.body
                 if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
                 and node.targets[0].id == "STUBS")
    stubs += """
struct ObOllamaUtils {
  struct ObOllamaEmbed {
    int parse_output(ObIAllocator &,ObJsonObject *,ObIJsonBase *&);
  };
};
#define UNUSED(x) (void)(x)
"""
    path = f"{AI}/ob_ai_func_utils.cpp"
    for label in ("baseline", "working"):
        source = (subprocess.check_output(["git", "show", f"{BASE}:{path}"], cwd=ROOT, text=True)
                  if label == "baseline" else (ROOT / path).read_text())
        code = section(source, "int ObOpenAIUtils::ObOpenAIEmbed::parse_output",
                       "int ObOllamaUtils::get_header")
        code += section(source, "int ObOllamaUtils::ObOllamaEmbed::parse_output",
                        "int ObDashscopeUtils::get_header")
        code += section(source, "int ObAIFuncJsonUtils::validate_embedding",
                        "int ObAIFuncModel::call_rerank")
        print(label, "provider response", flush=True)
        run = subprocess.run([str(build(tmp, label, stubs + code + PROTOCOL_TESTS))])
        if label == "working":
            run.check_returncode()


def http_checks(tmp):
    requests = []

    class Mock(BaseHTTPRequestHandler):
        def do_POST(self):
            data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append((data["model"], data["dimensions"], data["input"]))
            if data["input"] == ["fail"]:
                self.send_error(500)
                return
            self.send_response(200)
            self.end_headers()
            for value in data["input"]:
                vector = [int(value)] + [data["dimensions"]] * (data["dimensions"] - 1)
                self.wfile.write((json.dumps(vector, separators=(",", ":")) + "\n").encode())

        def log_message(self, *_args):
            pass

    source = (ROOT / AI / "ob_expr_ai_embed.cpp").read_text()
    code = section(source, "static int get_embedding_dimension", "static int request_embeddings")
    code += section(source, "static constexpr int64_t EMBEDDING_BATCH_SIZE", "int ObExprAIEmbed::cg_expr")
    binary = build(tmp, "batch_callback", SQL_STUBS + code + SQL_TESTS)
    with HTTPServer(("127.0.0.1", 0), Mock) as server:
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            subprocess.run([str(binary), str(server.server_port)], check=True)
        finally:
            server.shutdown()
            thread.join()
    active = [str(i) for i in range(45) if i not in {0, 7, 8, 9, 11, 31, 44}]
    assert requests == [
        ("A", 2, active[:32]), ("A", 2, active[32:]),
        ("A", 2, ["0", "2"]), ("B", 2, ["1"]), ("A", 3, ["3"]),
        ("A", 2, ["0"]), ("A", 2, ["fail"]),
    ], requests
    print(f"Loopback server saw {len(active)} active inputs in {2} POSTs (32 + {len(active)-32}); "
          f"all-NULL and already evaluated batches added no POSTs. Total POSTs: {len(requests)}.")


with tempfile.TemporaryDirectory(prefix="ai_embed_batch_") as directory:
    tmp = Path(directory)
    protocol_checks(tmp)
    http_checks(tmp)
