#!/usr/bin/env python3
"""Local renderer checks using verbatim production function bodies with API stubs.
NOT a seekdb unit/integration test: JSON parser, LOB, SQL typing and allocator
integration must be verified by the C++ and mysqltest suites separately.
"""
import pathlib, subprocess, tempfile, os
ROOT = pathlib.Path(__file__).resolve().parents[4]
SOURCE = 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp'
STUBS = r"""
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT -1
#define OB_ALLOCATE_MEMORY_FAILED -2
#define OB_BUF_NOT_ENOUGH -3
#define OB_SIZE_OVERFLOW -4
#define INIT_SUCC(x) int x = 0
#define OB_SUCC(x) ((x)==0)
#define OB_FAIL(x) ((ret=(x))!=0)
#define OB_ISNULL(x) (!(x))
#define OB_NOT_NULL(x) (!!(x))
#define LOG_USER_ERROR(...) ((void)0)
#define MEMCPY memcpy
#define MAX(a,b) std::max<int64_t>(a,b)
struct ObString {
 const char *p=nullptr; int32_t n=0;
 ObString()=default;
 ObString(const char *s):p(s),n(strlen(s)){}
 ObString(int64_t len,const char *s):p(s),n(len){}
 const char *ptr() const{return p;} int32_t length() const{return n;}
 bool empty() const{return n==0;} operator const char*() const{return p;}
 void assign_ptr(const char *s,int32_t len){p=s;n=len;}
};
struct ObIAllocator {
 std::vector<void*> blocks; bool fail=false;
 void *alloc(int64_t n){if(fail)return nullptr; auto p=malloc(n); blocks.push_back(p);return p;}
 ~ObIAllocator(){for(auto p:blocks)free(p);}
};
int ob_write_string(ObIAllocator &a,ObString s,ObString &d){auto p=(char*)a.alloc(std::max(1,s.length()));if(!p)return -2;memcpy(p,s.ptr(),s.length());d.assign_ptr(p,s.length());return 0;}
struct ObJsonBuffer {std::string s; explicit ObJsonBuffer(ObIAllocator*){} const char*ptr(){return s.data();}int64_t length(){return s.size();}ObString string(){return ObString(s.size(),s.data());}};
enum class ObJsonNodeType {J_STRING,J_OBJECT,J_ARRAY,J_NULL};
struct ObJsonNode {virtual ~ObJsonNode()=default;virtual ObJsonNodeType json_type()=0;virtual int print(ObJsonBuffer&,int){return -1;}};
struct ObJsonString:ObJsonNode {std::string s;explicit ObJsonString(std::string v):s(v){} ObJsonNodeType json_type(){return ObJsonNodeType::J_STRING;}ObString get_str(){return ObString(s.size(),s.data());}};
struct ObJsonArray:ObJsonNode {std::vector<ObJsonNode*> v; std::string raw="[]";ObJsonNodeType json_type(){return ObJsonNodeType::J_ARRAY;}uint64_t element_count(){return v.size();}ObJsonNode*get_value(uint64_t i){return i<v.size()?v[i]:nullptr;}int print(ObJsonBuffer&b,int){b.s=raw;return 0;}};
struct ObJsonObject:ObJsonNode {std::map<std::string,ObJsonNode*> v;std::string raw="{}";ObJsonNodeType json_type(){return ObJsonNodeType::J_OBJECT;}uint64_t element_count(){return v.size();}ObJsonNode*get_value(const char*k){auto i=v.find(k);return i==v.end()?nullptr:i->second;}int print(ObJsonBuffer&b,int){b.s=raw;return 0;}};
template<class T,int N>struct ObSEArray{std::vector<T> v;int push_back(T x){v.push_back(x);return 0;}int64_t count(){return v.size();}T&at(int64_t i){return v.at(i);}};
struct ObAIFuncPromptObjectUtils {static constexpr const char*prompt_template_key="template";static constexpr const char*prompt_args_key="args";static bool is_valid_prompt_object(ObJsonObject*);static int replace_all_str_args_in_template(ObIAllocator&,ObJsonObject*,ObString&);};
"""
TESTS = r"""
int failures=0, checks=0;
void check(bool ok,const char*name){++checks;if(!ok){++failures;std::cout<<"FAIL "<<name<<"\n";}}
void test(std::string t,std::vector<ObJsonNode*> values,std::string expected,int error=0,bool fail_alloc=false){
 ObIAllocator a;a.fail=fail_alloc;ObJsonString tmpl(t);ObJsonArray args;args.v=values;ObJsonObject obj;obj.v={{"template",&tmpl},{"args",&args}};ObString result;
 int ret=ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(a,&obj,result);
 check(ret==error && (error!=0 || std::string(result.ptr(),result.length())==expected),t.substr(0,70).c_str());
}
int main(){
 ObJsonString a("a sufficiently long {1}"), b("中文"), empty(""), nul(std::string("x\0y",3));
 test("{0}{1}{0}{0}",{&a,&b},a.s+b.s+a.s+a.s);
 test("{01}/{0}",{&a,&b},b.s+"/"+a.s);
 test("{0}{0}",{&empty},"");test("{0}{0}",{&nul},nul.s+nul.s);
 test("{} {-1} {x} {0 \\{0}",{&b},"{} {-1} {x} {0 \\中文");
 test("{1}",{&a},"",-1);test("{0}",{},"",-1);
 test("{999999999999999999999999999999999999999}",{&a},"",-1);
 test("",{},"",-1);test("literal",{},"literal");test("{0}",{&b},"",-2,true);
 ObJsonArray array;array.raw="[1, true, {\"k\": \"中文\"}]";ObJsonObject object;object.raw="{\"nested\": []}";
 test("{0}|{1}|{0}",{&array,&object},array.raw+"|"+object.raw+"|"+array.raw);
 ObJsonArray ea;ObJsonObject eo;test("{0}{1}",{&ea,&eo},"[]{}");
 ObJsonString inner_t("inner {0}");ObJsonArray inner_a;inner_a.v={&b};ObJsonObject inner;inner.v={{"template",&inner_t},{"args",&inner_a}};test("outer {0}{0}",{&inner},"outer inner 中文inner 中文");
 ObJsonString longarg(std::string(1024*1024,'z'));test("{0}{0}{0}{0}",{&longarg},longarg.s+longarg.s+longarg.s+longarg.s);
 test(std::string(1024*1024,'t')+"{0}",{&b},std::string(1024*1024,'t')+b.s);
 ObJsonObject invalid;ObIAllocator alloc;ObString result;check(-1==ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(alloc,&invalid,result),"invalid object");
 std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
"""
def bodies(s):
    a=s.index('bool ObAIFuncPromptObjectUtils::is_valid_prompt_object(')
    b=s.index('const ObString ObAIFuncModel::get_request_model_name()',a)
    return s[a:b]
with tempfile.TemporaryDirectory(prefix='ai_prompt_local_') as d:
    for version in ('baseline','working'):
        source=(subprocess.check_output(['git','show','5bce9e06d1281347ad61344e4b01fab9158c58a2:'+SOURCE],cwd=ROOT,text=True)
                if version=='baseline' else (ROOT/SOURCE).read_text())
        cpp=pathlib.Path(d)/(version+'.cpp');exe=pathlib.Path(d)/version
        cpp.write_text(STUBS+bodies(source)+TESTS)
        subprocess.run(['g++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(cpp),'-o',str(exe)],check=True)
        run=subprocess.run([str(exe)],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'},capture_output=True,text=True)
        print(version, 'exit',run.returncode,flush=True);print(run.stdout,run.stderr,flush=True)
        if version=='working' and run.returncode: raise SystemExit(run.returncode)
