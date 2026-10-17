// Minimal JSON value, parser and writer for the phone protocol's control
// messages. Small on purpose: objects keep insertion order, numbers are
// doubles, strings are UTF-8.
#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace json
{
class Value
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : m_type(Type::Bool), m_bool(b) {}
    Value(int v) : m_type(Type::Number), m_num(v) {}
    Value(uint32_t v) : m_type(Type::Number), m_num(v) {}
    Value(int64_t v) : m_type(Type::Number), m_num(static_cast<double>(v)) {}
    Value(double v) : m_type(Type::Number), m_num(v) {}
    Value(const char* s) : m_type(Type::String), m_str(s) {}
    Value(std::string s) : m_type(Type::String), m_str(std::move(s)) {}
    Value(Array a) : m_type(Type::Array), m_arr(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : m_type(Type::Object), m_obj(std::make_shared<Object>(std::move(o))) {}

    static Value MakeObject() { return Value(Object{}); }
    static Value MakeArray() { return Value(Array{}); }

    Type type() const { return m_type; }
    bool IsNull() const { return m_type == Type::Null; }
    bool IsObject() const { return m_type == Type::Object; }
    bool IsArray() const { return m_type == Type::Array; }
    bool IsString() const { return m_type == Type::String; }
    bool IsNumber() const { return m_type == Type::Number; }

    bool Bool(bool fallback = false) const { return m_type == Type::Bool ? m_bool : fallback; }
    double Num(double fallback = 0) const { return m_type == Type::Number ? m_num : fallback; }
    int Int(int fallback = 0) const { return m_type == Type::Number ? static_cast<int>(std::lround(m_num)) : fallback; }
    const std::string& Str() const { static const std::string empty; return m_type == Type::String ? m_str : empty; }
    std::string Str(const std::string& fallback) const { return m_type == Type::String ? m_str : fallback; }

    const Array& Items() const { static const Array empty; return m_arr ? *m_arr : empty; }
    const Object& Members() const { static const Object empty; return m_obj ? *m_obj : empty; }

    // Object access: missing keys read as null.
    const Value& operator[](const std::string& key) const
    {
        static const Value null;
        if (m_obj)
            for (const auto& [k, v] : *m_obj)
                if (k == key) return v;
        return null;
    }

    Value& Set(const std::string& key, Value v)
    {
        if (!m_obj) { m_type = Type::Object; m_obj = std::make_shared<Object>(); }
        for (auto& [k, existing] : *m_obj)
            if (k == key) { existing = std::move(v); return *this; }
        m_obj->emplace_back(key, std::move(v));
        return *this;
    }

    Value& Push(Value v)
    {
        if (!m_arr) { m_type = Type::Array; m_arr = std::make_shared<Array>(); }
        m_arr->push_back(std::move(v));
        return *this;
    }

    std::string Dump() const
    {
        std::string out;
        Write(out);
        return out;
    }

    static Value Parse(const std::string& text)
    {
        size_t pos = 0;
        Value v = ParseValue(text, pos);
        SkipWs(text, pos);
        if (pos != text.size()) throw std::runtime_error("json: trailing data");
        return v;
    }

private:
    void Write(std::string& out) const
    {
        switch (m_type)
        {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += m_bool ? "true" : "false"; break;
        case Type::Number:
        {
            char buf[32];
            if (std::isfinite(m_num) && m_num == std::floor(m_num) && std::fabs(m_num) < 1e15)
                snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(m_num));
            else
                snprintf(buf, sizeof(buf), "%.9g", std::isfinite(m_num) ? m_num : 0.0);
            out += buf;
            break;
        }
        case Type::String: WriteString(out, m_str); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < m_arr->size(); ++i) { if (i) out += ','; (*m_arr)[i].Write(out); }
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < m_obj->size(); ++i)
            {
                if (i) out += ',';
                WriteString(out, (*m_obj)[i].first);
                out += ':';
                (*m_obj)[i].second.Write(out);
            }
            out += '}';
            break;
        }
    }

    static void WriteString(std::string& out, const std::string& s)
    {
        out += '"';
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
                else out += static_cast<char>(c);
            }
        }
        out += '"';
    }

    static void SkipWs(const std::string& t, size_t& p)
    {
        while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) ++p;
    }

    static Value ParseValue(const std::string& t, size_t& p)
    {
        SkipWs(t, p);
        if (p >= t.size()) throw std::runtime_error("json: unexpected end");
        const char c = t[p];
        if (c == '{')
        {
            Value v = MakeObject();
            ++p;
            SkipWs(t, p);
            if (p < t.size() && t[p] == '}') { ++p; return v; }
            for (;;)
            {
                SkipWs(t, p);
                std::string key = ParseString(t, p);
                SkipWs(t, p);
                if (p >= t.size() || t[p++] != ':') throw std::runtime_error("json: expected ':'");
                v.Set(key, ParseValue(t, p));
                SkipWs(t, p);
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == '}') { ++p; return v; }
                throw std::runtime_error("json: expected ',' or '}'");
            }
        }
        if (c == '[')
        {
            Value v = MakeArray();
            ++p;
            SkipWs(t, p);
            if (p < t.size() && t[p] == ']') { ++p; return v; }
            for (;;)
            {
                v.Push(ParseValue(t, p));
                SkipWs(t, p);
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == ']') { ++p; return v; }
                throw std::runtime_error("json: expected ',' or ']'");
            }
        }
        if (c == '"') return Value(ParseString(t, p));
        if (t.compare(p, 4, "true") == 0) { p += 4; return Value(true); }
        if (t.compare(p, 5, "false") == 0) { p += 5; return Value(false); }
        if (t.compare(p, 4, "null") == 0) { p += 4; return Value(); }
        size_t end = p;
        while (end < t.size() && (isdigit(static_cast<unsigned char>(t[end])) || strchr("+-.eE", t[end]))) ++end;
        if (end == p) throw std::runtime_error("json: bad token");
        const double d = std::stod(t.substr(p, end - p));
        p = end;
        return Value(d);
    }

    static std::string ParseString(const std::string& t, size_t& p)
    {
        if (p >= t.size() || t[p] != '"') throw std::runtime_error("json: expected string");
        ++p;
        std::string s;
        while (p < t.size() && t[p] != '"')
        {
            char c = t[p++];
            if (c != '\\') { s += c; continue; }
            if (p >= t.size()) break;
            const char e = t[p++];
            switch (e)
            {
            case 'n': s += '\n'; break;
            case 'r': s += '\r'; break;
            case 't': s += '\t'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'u':
            {
                unsigned cp = static_cast<unsigned>(std::stoul(t.substr(p, 4), nullptr, 16));
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && t.compare(p, 2, "\\u") == 0)
                {
                    const unsigned lo = static_cast<unsigned>(std::stoul(t.substr(p + 2, 4), nullptr, 16));
                    p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                // UTF-8 encode
                if (cp < 0x80) s += static_cast<char>(cp);
                else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
                else { s += static_cast<char>(0xF0 | (cp >> 18)); s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
                break;
            }
            default: s += e;
            }
        }
        if (p >= t.size()) throw std::runtime_error("json: unterminated string");
        ++p;
        return s;
    }

    Type m_type = Type::Null;
    bool m_bool = false;
    double m_num = 0;
    std::string m_str;
    std::shared_ptr<Array> m_arr;
    std::shared_ptr<Object> m_obj;
};
} // namespace json
