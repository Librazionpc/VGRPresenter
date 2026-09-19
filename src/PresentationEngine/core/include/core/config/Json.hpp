#pragma once

// Minimal, dependency-free JSON implementation used by the ConfigurationManager
// (docs/specs/03). Supports the full JSON grammar for parsing and compact
// serialization. Numbers are stored as double (config precision is sufficient).

#include "core/common/Common.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <variant>
#include <vector>

namespace bps::json {

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    enum class Type : int { Null, Bool, Number, String, Array, Object };    Value() = default;
    // Converting constructors: containers assign directly to Value. (The static
    // factories for Array/Object are intentionally absent — a member type and a
    // member function cannot share a name in the same class.)
    Value(Array arr) : t_(Type::Array), v_(std::move(arr)) {}
    Value(Object obj) : t_(Type::Object), v_(std::move(obj)) {}

    static Value Null() {
        return {};
    }
    static Value Bool(bool b) {
        Value v;
        v.t_ = Type::Bool;
        v.v_ = b;
        return v;
    }
    static Value Number(double d) {
        Value v;
        v.t_ = Type::Number;
        v.v_ = d;
        return v;
    }
    static Value String(std::string s) {
        Value v;
        v.t_ = Type::String;
        v.v_ = std::move(s);
        return v;
    }

    Type type() const noexcept { return t_; }
    bool isNull() const noexcept { return t_ == Type::Null; }

    bool asBool(bool dflt = false) const {
        if (t_ != Type::Bool) return dflt;
        return std::get<bool>(v_);
    }
    double asNumber(double dflt = 0.0) const {
        if (t_ != Type::Number) return dflt;
        return std::get<double>(v_);
    }
    long long asInt(long long dflt = 0) const {
        if (t_ != Type::Number) return dflt;
        return static_cast<long long>(std::get<double>(v_));
    }
    std::string_view asString(std::string_view dflt = {}) const {
        if (t_ != Type::String) return dflt;
        return std::get<std::string>(v_);
    }
    const Array* asArray() const { return t_ == Type::Array ? &std::get<Array>(v_) : nullptr; }
    const Object* asObject() const { return t_ == Type::Object ? &std::get<Object>(v_) : nullptr; }

    // Dot-path lookup, e.g. Find("video.bitrate"). Returns nullptr on miss.
    const Value* Find(std::string_view dotPath) const {
        const Value* node = this;
        size_t start = 0;
        while (node && start <= dotPath.size()) {
            size_t dot = dotPath.find('.', start);
            if (dot == std::string_view::npos) dot = dotPath.size();
            std::string_view key = dotPath.substr(start, dot - start);
            if (key.empty()) return nullptr;
            if (node->t_ != Type::Object) return nullptr;
            auto it = std::get<Object>(node->v_).find(key);
            if (it == std::get<Object>(node->v_).end()) return nullptr;
            node = &it->second;
            start = dot + 1;
            if (dot == dotPath.size()) break;
        }
        return node;
    }
    bool Has(std::string_view dotPath) const { return Find(dotPath) != nullptr; }

    std::string ToString() const;

private:
    Type t_ = Type::Null;
    std::variant<bool, double, std::string, Array, Object> v_;
};

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------
namespace detail {

inline void EscapeInto(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

inline void SerializeInto(const Value& v, std::string& out) {
    switch (v.type()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
        case Value::Type::Number: {
            double d = v.asNumber();
            if (d == std::floor(d) && std::abs(d) < 1e15) {
                out += std::to_string(static_cast<long long>(d));
            } else {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.10g", d);
                out += buf;
            }
            break;
        }
        case Value::Type::String: {
            out += '"';
            EscapeInto(out, v.asString());
            out += '"';
            break;
        }
        case Value::Type::Array: {
            out += '[';
            bool first = true;
            for (const auto& e : *v.asArray()) {
                if (!first) out += ',';
                first = false;
                SerializeInto(e, out);
            }
            out += ']';
            break;
        }
        case Value::Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& [k, val] : *v.asObject()) {
                if (!first) out += ',';
                first = false;
                out += '"';
                EscapeInto(out, k);
                out += "\":";
                SerializeInto(val, out);
            }
            out += '}';
            break;
        }
    }
}

} // namespace detail

inline std::string Value::ToString() const {
    std::string out;
    detail::SerializeInto(*this, out);
    return out;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
namespace detail {

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Result<Value> ParseDocument() {
        auto v = ParseValue();
        if (!v.ok()) return v.error();
        SkipWs();
        if (pos_ != s_.size())
            return Error::Make(Err::ParseError, "Json",
                               "trailing characters at offset " + std::to_string(pos_));
        return v;
    }

private:
    Error Err(std::string msg) {
        return Error::Make(Err::ParseError, "Json",
                           msg + " at offset " + std::to_string(pos_));
    }

    void SkipWs() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }

    bool Consume(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    Result<Value> ParseValue() {
        SkipWs();
        if (pos_ >= s_.size()) return Err("unexpected end of input");
        char c = s_[pos_];
        switch (c) {
            case '{': return ParseObject();
            case '[': return ParseArray();
            case '"': return ParseString();
            case 't': return ParseLiteral("true", Value::Bool(true));
            case 'f': return ParseLiteral("false", Value::Bool(false));
            case 'n': return ParseLiteral("null", Value::Null());
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber();
                return Err("unexpected character '" + std::string(1, c) + "'");
        }
    }

    Result<Value> ParseLiteral(const char* lit, Value v) {
        size_t n = std::char_traits<char>::length(lit);
        if (s_.substr(pos_, n) != lit) return Err("invalid literal");
        pos_ += n;
        return v;
    }

    Result<Value> ParseString() {
        if (!Consume('"')) return Err("expected '\"'");
        std::string out;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') {
                Value v = Value::String(std::move(out));
                return v;
            }
            if (c == '\\') {
                if (pos_ >= s_.size()) return Err("unterminated escape");
                char e = s_[pos_++];
                switch (e) {
                    case '"':  out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (pos_ + 4 > s_.size()) return Err("bad \\u escape");
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s_[pos_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else return Err("bad \\u escape digit");
                        }
                        // Encode as UTF-8.
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return Err("invalid escape '\\" + std::string(1, e) + "'");
                }
            } else {
                out += c;
            }
        }
        return Err("unterminated string");
    }

    Result<Value> ParseNumber() {
        size_t start = pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
                ++pos_;
            else
                break;
        }
        std::string token(s_.substr(start, pos_ - start));
        if (token.empty() || token == "-") return Err("invalid number");
        char* end = nullptr;
        double d = std::strtod(token.c_str(), &end);
        if (end == token.c_str()) return Err("invalid number");
        return Value::Number(d);
    }

    Result<Value> ParseArray() {
        if (!Consume('[')) return Err("expected '['");
        Value::Array arr;
        SkipWs();
        if (Consume(']')) return Value(std::move(arr));
        while (true) {
            SkipWs();
            auto v = ParseValue();
            if (!v.ok()) return v.error();
            arr.push_back(std::move(v.value()));
            SkipWs();
            if (Consume(']')) break;
            if (!Consume(',')) return Err("expected ',' or ']' in array");
        }
        return Value(std::move(arr));
    }

    Result<Value> ParseObject() {
        if (!Consume('{')) return Err("expected '{'");
        Value::Object obj;
        SkipWs();
        if (Consume('}')) return Value(std::move(obj));
        while (true) {
            SkipWs();
            auto key = ParseString();
            if (!key.ok()) return key.error();
            SkipWs();
            if (!Consume(':')) return Err("expected ':' after object key");
            SkipWs();
            auto val = ParseValue();
            if (!val.ok()) return val.error();
            obj[std::string(key.value().asString())] = std::move(val.value());
            SkipWs();
            if (Consume('}')) break;
            if (!Consume(',')) return Err("expected ',' or '}' in object");
        }
        return Value(std::move(obj));
    }

    std::string_view s_;
    size_t pos_ = 0;
};

} // namespace detail

inline Result<Value> Parse(std::string_view text) {
    detail::Parser p(text);
    return p.ParseDocument();
}

} // namespace bps::json
