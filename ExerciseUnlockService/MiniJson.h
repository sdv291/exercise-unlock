#pragma once

// Just enough JSON for the relay protocol (PhoneLink): a small DOM parser
// with depth / size limits and a streaming writer. Strings are UTF-8.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace ExerciseUnlock::Svc::Json
{
    struct Value
    {
        enum class Type { Null, Bool, Number, String, Array, Object };
        Type        type = Type::Null;
        bool        b = false;
        double      n = 0.0;
        std::string s;
        std::vector<Value> arr;
        std::vector<std::pair<std::string, Value>> obj;

        bool IsObject() const { return type == Type::Object; }
        const Value* Get(const char* key) const
        {
            if (type != Type::Object) return nullptr;
            for (const auto& kv : obj)
                if (kv.first == key) return &kv.second;
            return nullptr;
        }
        std::string Str(const char* key, const char* dflt = "") const
        {
            const Value* v = Get(key);
            return (v && v->type == Type::String) ? v->s : std::string(dflt);
        }
        double Num(const char* key, double dflt = 0.0) const
        {
            const Value* v = Get(key);
            return (v && v->type == Type::Number) ? v->n : dflt;
        }
    };

    class Parser
    {
    public:
        static bool Parse(const std::string& text, Value& out)
        {
            if (text.size() > 64 * 1024) return false;
            Parser p(text);
            p.Ws();
            if (!p.ParseValue(out, 0)) return false;
            p.Ws();
            return p.m_pos == text.size();
        }

    private:
        explicit Parser(const std::string& t) : m_t(t) {}

        bool ParseValue(Value& v, int depth)
        {
            if (depth > 16 || m_pos >= m_t.size()) return false;
            const char c = m_t[m_pos];
            if (c == '{') return ParseObject(v, depth);
            if (c == '[') return ParseArray(v, depth);
            if (c == '"') { v.type = Value::Type::String; return ParseString(v.s); }
            if (Lit("true"))  { v.type = Value::Type::Bool; v.b = true;  return true; }
            if (Lit("false")) { v.type = Value::Type::Bool; v.b = false; return true; }
            if (Lit("null"))  { v.type = Value::Type::Null; return true; }
            return ParseNumber(v);
        }

        bool ParseObject(Value& v, int depth)
        {
            v.type = Value::Type::Object;
            ++m_pos;
            Ws();
            if (Peek('}')) { ++m_pos; return true; }
            for (;;)
            {
                std::string key;
                Ws();
                if (!Peek('"') || !ParseString(key)) return false;
                Ws();
                if (!Peek(':')) return false;
                ++m_pos;
                Ws();
                Value child;
                if (!ParseValue(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                Ws();
                if (Peek(',')) { ++m_pos; continue; }
                if (Peek('}')) { ++m_pos; return true; }
                return false;
            }
        }

        bool ParseArray(Value& v, int depth)
        {
            v.type = Value::Type::Array;
            ++m_pos;
            Ws();
            if (Peek(']')) { ++m_pos; return true; }
            for (;;)
            {
                Ws();
                Value child;
                if (!ParseValue(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                Ws();
                if (Peek(',')) { ++m_pos; continue; }
                if (Peek(']')) { ++m_pos; return true; }
                return false;
            }
        }

        bool ParseString(std::string& out)
        {
            ++m_pos;   // opening quote
            while (m_pos < m_t.size())
            {
                const char c = m_t[m_pos++];
                if (c == '"') return true;
                if (static_cast<unsigned char>(c) < 0x20) return false;
                if (c != '\\') { out += c; continue; }
                if (m_pos >= m_t.size()) return false;
                const char e = m_t[m_pos++];
                switch (e)
                {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                {
                    uint32_t cp = 0;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF)
                    {
                        uint32_t lo = 0;
                        if (m_pos + 2 > m_t.size() || m_t[m_pos] != '\\' || m_t[m_pos + 1] != 'u') return false;
                        m_pos += 2;
                        if (!Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    Utf8(cp, out);
                    break;
                }
                default:
                    return false;
                }
            }
            return false;
        }

        bool Hex4(uint32_t& cp)
        {
            if (m_pos + 4 > m_t.size()) return false;
            cp = 0;
            for (int i = 0; i < 4; ++i)
            {
                const char h = m_t[m_pos++];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= static_cast<uint32_t>(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= static_cast<uint32_t>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= static_cast<uint32_t>(h - 'A' + 10);
                else return false;
            }
            return true;
        }

        static void Utf8(uint32_t cp, std::string& out)
        {
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000)
            {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        bool ParseNumber(Value& v)
        {
            const size_t start = m_pos;
            if (Peek('-')) ++m_pos;
            while (m_pos < m_t.size())
            {
                const char c = m_t[m_pos];
                if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') ++m_pos;
                else break;
            }
            if (m_pos == start) return false;
            const std::string num = m_t.substr(start, m_pos - start);
            char* end = nullptr;
            v.n = std::strtod(num.c_str(), &end);
            if (!end || *end != '\0') return false;
            v.type = Value::Type::Number;
            return true;
        }

        bool Lit(const char* word)
        {
            size_t i = 0;
            while (word[i] && m_pos + i < m_t.size() && m_t[m_pos + i] == word[i]) ++i;
            if (word[i] != '\0') return false;
            m_pos += i;
            return true;
        }
        bool Peek(char c) const { return m_pos < m_t.size() && m_t[m_pos] == c; }
        void Ws()
        {
            while (m_pos < m_t.size() &&
                   (m_t[m_pos] == ' ' || m_t[m_pos] == '\t' || m_t[m_pos] == '\n' || m_t[m_pos] == '\r'))
                ++m_pos;
        }

        const std::string& m_t;
        size_t             m_pos = 0;
    };

    // Streaming writer: commas are inserted automatically.
    //   Writer w; w.BeginObject().Key("t").String("rep").EndObject();
    class Writer
    {
    public:
        Writer& BeginObject() { Sep(); m_s += '{'; m_first.push_back(true); return *this; }
        Writer& EndObject()   { m_s += '}'; m_first.pop_back(); return *this; }
        Writer& BeginArray()  { Sep(); m_s += '['; m_first.push_back(true); return *this; }
        Writer& EndArray()    { m_s += ']'; m_first.pop_back(); return *this; }
        Writer& Key(const char* k) { Sep(); Quote(k); m_s += ':'; m_afterKey = true; return *this; }
        Writer& String(const std::string& v) { Sep(); Quote(v); return *this; }
        Writer& Bool(bool v) { Sep(); m_s += v ? "true" : "false"; return *this; }
        Writer& Int(long long v) { Sep(); m_s += std::to_string(v); return *this; }
        Writer& Number(double v)
        {
            Sep();
            char buf[32]{};
            std::snprintf(buf, sizeof(buf), "%.6g", v);
            m_s += buf;
            return *this;
        }
        // Splice in an already-serialized value.
        Writer& Raw(const std::string& json) { Sep(); m_s += json; return *this; }
        const std::string& Str() const { return m_s; }

    private:
        void Sep()
        {
            if (m_afterKey) { m_afterKey = false; return; }
            if (!m_first.empty())
            {
                if (!m_first.back()) m_s += ',';
                m_first.back() = false;
            }
        }
        void Quote(const std::string& v)
        {
            m_s += '"';
            for (const char ch : v)
            {
                const unsigned char c = static_cast<unsigned char>(ch);
                switch (c)
                {
                case '"':  m_s += "\\\""; break;
                case '\\': m_s += "\\\\"; break;
                case '\n': m_s += "\\n"; break;
                case '\r': m_s += "\\r"; break;
                case '\t': m_s += "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8]{};
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        m_s += buf;
                    }
                    else m_s += ch;
                }
            }
            m_s += '"';
        }

        std::string       m_s;
        std::vector<bool> m_first;
        bool              m_afterKey = false;
    };
}
