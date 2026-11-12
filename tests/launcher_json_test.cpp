#include "../launcher/src/launcher_json.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace launcher;
void check(bool x,const char* s){if(!x)throw std::runtime_error(s);}
int main(){try{
    JSONValue v;std::string error;
    check(parse_json(R"({"z":[null,true,false,-9223372036854775808,1.25e2],"a":"\uD83D\uDE00\n\t\"\\","nul":"a\u0000b"})",v,error),"valid JSON/Unicode");
    int64_t n=0;check(v.find("z")->array()->at(3).integer(n)&&n==std::numeric_limits<int64_t>::min(),"exact integer range");
    check(*v.find("a")->string()==std::string("\xf0\x9f\x98\x80\n\t\"\\"),"Unicode surrogate decoding");
    check(v.find("nul")->string()->size()==3,"escaped NUL retained");
    const auto canonical=write_json(v);JSONValue roundtrip;check(parse_json(canonical,roundtrip,error)&&write_json(roundtrip)==canonical,"deterministic roundtrip");
    check(parse_json(write_json(v,true),roundtrip,error),"pretty JSON roundtrip");
    for(const auto s:{"", "01","-01","+1",".1","1.","1e","1e+","1e9999","9223372036854775808","[1,]","{\"a\":1,}","{\"a\":1,\"a\":2}","{\"a\":1,\"\\u0061\":2}","true false","\"\\uD800\"","\"\\uDC00\"","\"\\uD800\\u0041\"","\"\\q\""}){
        JSONValue keep("unchanged");check(!parse_json(s,keep,error)&&*keep.string()=="unchanged"&&!error.empty(),"strict malformed JSON rejection/output preservation");
    }
    for(const auto& s:{std::string("\"\xc0\xaf\""),std::string("\"\xed\xa0\x80\""),std::string("\"a\nb\"")})check(!parse_json(s,roundtrip,error),"invalid UTF-8/control rejection");
    JSONLimits limits;limits.max_bytes=2;check(!parse_json("null",roundtrip,error,limits),"byte bound");limits={};limits.max_string_bytes=2;check(!parse_json("\"abc\"",roundtrip,error,limits),"string bound");limits={};limits.max_nodes=2;check(!parse_json("[0,1]",roundtrip,error,limits),"node bound");limits={};limits.max_depth=1;check(!parse_json("[[0]]",roundtrip,error,limits),"depth bound");
    bool threw=false;try{write_json(JSONValue(std::numeric_limits<double>::infinity()));}catch(const std::invalid_argument&){threw=true;}check(threw,"writer nonfinite rejection");threw=false;try{write_json(JSONValue(std::string("\xc0\xaf")));}catch(const std::invalid_argument&){threw=true;}check(threw,"writer UTF-8 rejection");
    std::cout<<"Strict launcher JSON parsing/writing, UTF-8, numeric limits, duplicate keys, budgets and output preservation passed.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
