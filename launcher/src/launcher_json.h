#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace launcher {
struct JSONValue {
    using Array=std::vector<JSONValue>;
    using Object=std::map<std::string,JSONValue,std::less<>>;
    using Value=std::variant<std::nullptr_t,bool,int64_t,double,std::string,Array,Object>;
    Value value=nullptr;
    JSONValue()=default;
    JSONValue(std::nullptr_t):value(nullptr){}
    JSONValue(bool v):value(v){}
    JSONValue(int v):value(int64_t(v)){}
    JSONValue(int64_t v):value(v){}
    JSONValue(double v):value(v){}
    JSONValue(const char* v):value(std::string(v)){}
    JSONValue(std::string v):value(std::move(v)){}
    JSONValue(Array v):value(std::move(v)){}
    JSONValue(Object v):value(std::move(v)){}
    const JSONValue* find(std::string_view key) const;
    const std::string* string() const {return std::get_if<std::string>(&value);}
    const Array* array() const {return std::get_if<Array>(&value);}
    const Object* object() const {return std::get_if<Object>(&value);}
    Array* array(){return std::get_if<Array>(&value);}
    Object* object(){return std::get_if<Object>(&value);}
    bool integer(int64_t& out) const;
    bool boolean(bool& out) const;
};
struct JSONLimits {
    size_t max_bytes=16u*1024*1024;
    size_t max_string_bytes=1024u*1024;
    size_t max_nodes=100000;
    unsigned max_depth=32;
};
// Strict UTF-8 JSON: rejects duplicate keys, malformed escapes/numbers and
// trailing input. On failure `out` is unchanged; errors contain byte offsets.
bool parse_json(std::string_view input,JSONValue& out,std::string& error,JSONLimits limits={});
// Deterministic object order, valid UTF-8 and escaped control characters.
// Throws std::invalid_argument for invalid UTF-8/non-finite numeric values.
std::string write_json(const JSONValue&,bool pretty=false);
}
