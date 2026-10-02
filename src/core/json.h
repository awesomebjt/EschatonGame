#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Minimal JSON DOM, enough for manifest.json. Not a general-purpose library:
// no \u surrogate pairs, no streaming, whole document parsed into memory.
namespace json {

struct Value
{
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Type        type   = Type::Null;
    bool        boolean = false;
    double      number = 0.0;
    std::string string;
    std::vector<Value>       items;   // Array elements, or Object values
    std::vector<std::string> keys;    // Object keys, parallel to `items`

    bool is_null()   const { return type == Type::Null; }
    bool is_number() const { return type == Type::Number; }
    bool is_array()  const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    // Missing keys and out-of-range indices return a shared null value, so
    // lookups chain without checks: `m["map"]["width"].as_double()`.
    const Value& operator[](std::string_view key) const;
    const Value& operator[](size_t index) const;
    size_t       size() const { return items.size(); }

    double             as_double(double fallback = 0.0) const;
    int                as_int(int fallback = 0) const;
    bool               as_bool(bool fallback = false) const;
    const std::string& as_string() const;
};

std::optional<Value> parse(std::string_view text);

} // namespace json
