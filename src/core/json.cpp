#include "core/json.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace json {

static const Value      k_null{};
static const std::string k_empty{};

const Value& Value::operator[](std::string_view key) const
{
    if (type != Type::Object) return k_null;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return items[i];
    return k_null;
}

const Value& Value::operator[](size_t index) const
{
    if (type != Type::Array || index >= items.size()) return k_null;
    return items[index];
}

double Value::as_double(double fallback) const
{
    return type == Type::Number ? number : fallback;
}

int Value::as_int(int fallback) const
{
    return type == Type::Number ? static_cast<int>(std::lround(number)) : fallback;
}

bool Value::as_bool(bool fallback) const
{
    return type == Type::Bool ? boolean : fallback;
}

const std::string& Value::as_string() const
{
    return type == Type::String ? string : k_empty;
}

namespace {

struct Parser
{
    std::string_view s;
    size_t           pos = 0;
    int              depth = 0;

    void skip_ws()
    {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
            ++pos;
    }

    bool consume(char c)
    {
        skip_ws();
        if (pos < s.size() && s[pos] == c) { ++pos; return true; }
        return false;
    }

    bool literal(std::string_view word)
    {
        if (s.substr(pos, word.size()) != word) return false;
        pos += word.size();
        return true;
    }

    bool parse_string(std::string& out)
    {
        if (!consume('"')) return false;
        while (pos < s.size()) {
            const char c = s[pos++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (pos >= s.size()) return false;
            const char e = s[pos++];
            switch (e) {
                case '"': case '\\': case '/': out.push_back(e); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 > s.size()) return false;
                    const unsigned cp = static_cast<unsigned>(
                        std::strtoul(std::string(s.substr(pos, 4)).c_str(), nullptr, 16));
                    pos += 4;
                    // UTF-8 encode the BMP code point; surrogate pairs are not combined.
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool parse_value(Value& v)
    {
        if (++depth > 64) return false;
        skip_ws();
        if (pos >= s.size()) return false;

        bool ok = false;
        const char c = s[pos];
        if (c == '{') {
            ++pos;
            v.type = Value::Type::Object;
            if (consume('}')) { ok = true; }
            else {
                do {
                    std::string key;
                    skip_ws();
                    if (!parse_string(key) || !consume(':')) { --depth; return false; }
                    v.keys.push_back(std::move(key));
                    v.items.emplace_back();
                    if (!parse_value(v.items.back())) { --depth; return false; }
                } while (consume(','));
                ok = consume('}');
            }
        } else if (c == '[') {
            ++pos;
            v.type = Value::Type::Array;
            if (consume(']')) { ok = true; }
            else {
                do {
                    v.items.emplace_back();
                    if (!parse_value(v.items.back())) { --depth; return false; }
                } while (consume(','));
                ok = consume(']');
            }
        } else if (c == '"') {
            v.type = Value::Type::String;
            ok = parse_string(v.string);
        } else if (c == 't') {
            v.type = Value::Type::Bool; v.boolean = true;  ok = literal("true");
        } else if (c == 'f') {
            v.type = Value::Type::Bool; v.boolean = false; ok = literal("false");
        } else if (c == 'n') {
            ok = literal("null");
        } else {
            // strtod needs a terminated buffer; numbers are short, so copy the token.
            size_t end = pos;
            while (end < s.size() && s[end] != '\0'
                   && (std::strchr("+-.eE", s[end]) || (s[end] >= '0' && s[end] <= '9')))
                ++end;
            if (end == pos) { --depth; return false; }
            const std::string token(s.substr(pos, end - pos));
            char* stop = nullptr;
            v.type   = Value::Type::Number;
            v.number = std::strtod(token.c_str(), &stop);
            ok  = stop == token.c_str() + token.size();
            pos = end;
        }
        --depth;
        return ok;
    }
};

} // namespace

std::optional<Value> parse(std::string_view text)
{
    Parser p{text};
    Value  root;
    if (!p.parse_value(root)) return std::nullopt;
    p.skip_ws();
    if (p.pos != text.size()) return std::nullopt;
    return root;
}

} // namespace json
