#pragma once
/**
 * Minimal JSON value type + parser/serialiser.
 * Shared by the LSP and DAP servers; lives in eta_core so both can
 * include it without duplicating code.
 */

#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace eta::json {

class Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

class Value {
public:
    using Data = std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object>;
    Data data{nullptr};

    Value() = default;
    Value(std::nullptr_t)       : data(nullptr) {}
    Value(bool b)               : data(b) {}
    Value(int v)                : data(static_cast<int64_t>(v)) {}
    Value(int64_t v)            : data(v) {}
    Value(std::size_t v)        : data(static_cast<int64_t>(v)) {}
    Value(double v)             : data(v) {}
    Value(const char* s)        : data(std::string(s)) {}
    Value(std::string s)        : data(std::move(s)) {}
    Value(Array a)              : data(std::move(a)) {}
    Value(Object o)             : data(std::move(o)) {}

    bool is_null()   const { return std::holds_alternative<std::nullptr_t>(data); }
    bool is_bool()   const { return std::holds_alternative<bool>(data); }
    bool is_int()    const { return std::holds_alternative<int64_t>(data); }
    bool is_double() const { return std::holds_alternative<double>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_array()  const { return std::holds_alternative<Array>(data); }
    bool is_object() const { return std::holds_alternative<Object>(data); }

    bool               as_bool()   const { return std::get<bool>(data); }
    int64_t            as_int()    const { return std::get<int64_t>(data); }
    double             as_double() const { return std::get<double>(data); }
    const std::string& as_string() const { return std::get<std::string>(data); }
    const Array&       as_array()  const { return std::get<Array>(data); }
    const Object&      as_object() const { return std::get<Object>(data); }
    Object&            as_object()       { return std::get<Object>(data); }

    /// Convenience accessors for objects
    const Value& operator[](const std::string& key) const {
        static const Value null_val{};
        if (!is_object()) return null_val;
        auto it = std::get<Object>(data).find(key);
        return it != std::get<Object>(data).end() ? it->second : null_val;
    }

    std::optional<std::string> get_string(const std::string& key) const {
        if (!is_object()) return std::nullopt;
        auto it = std::get<Object>(data).find(key);
        if (it == std::get<Object>(data).end() || !it->second.is_string()) return std::nullopt;
        return it->second.as_string();
    }

    std::optional<int64_t> get_int(const std::string& key) const {
        if (!is_object()) return std::nullopt;
        auto it = std::get<Object>(data).find(key);
        if (it == std::get<Object>(data).end() || !it->second.is_int()) return std::nullopt;
        return it->second.as_int();
    }

    bool has(const std::string& key) const {
        if (!is_object()) return false;
        return std::get<Object>(data).contains(key);
    }
};

inline void escape_string(std::ostream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b";  break;
            case '\f': os << "\\f";  break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    os << buf;
                } else {
                    os << c;
                }
        }
    }
    os << '"';
}

inline void serialize(std::ostream& os, const Value& v) {
    std::visit([&](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
            os << "null";
        } else if constexpr (std::is_same_v<T, bool>) {
            os << (val ? "true" : "false");
        } else if constexpr (std::is_same_v<T, int64_t>) {
            os << val;
        } else if constexpr (std::is_same_v<T, double>) {
            os << val;
        } else if constexpr (std::is_same_v<T, std::string>) {
            escape_string(os, val);
        } else if constexpr (std::is_same_v<T, Array>) {
            os << '[';
            for (std::size_t i = 0; i < val.size(); ++i) {
                if (i > 0) os << ',';
                serialize(os, val[i]);
            }
            os << ']';
        } else if constexpr (std::is_same_v<T, Object>) {
            os << '{';
            bool first = true;
            for (const auto& [k, v2] : val) {
                if (!first) os << ',';
                first = false;
                escape_string(os, k);
                os << ':';
                serialize(os, v2);
            }
            os << '}';
        }
    }, v.data);
}

inline std::string to_string(const Value& v) {
    std::ostringstream os;
    serialize(os, v);
    return os.str();
}

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input), pos_(0) {}

    Value parse() {
        skip_ws();
        auto v = parse_value();
        skip_ws();
        return v;
    }

private:
    std::string_view input_;
    std::size_t pos_;

    [[noreturn]] void error(const std::string& msg) {
        throw ParseError("JSON parse error at " + std::to_string(pos_) + ": " + msg);
    }

    char peek() const { return pos_ < input_.size() ? input_[pos_] : '\0'; }
    char advance() { return pos_ < input_.size() ? input_[pos_++] : '\0'; }

    void skip_ws() {
        while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\t' ||
                                         input_[pos_] == '\n' || input_[pos_] == '\r'))
            ++pos_;
    }

    void expect(char c) {
        skip_ws();
        if (advance() != c)
            error(std::string("expected '") + c + "'");
    }

    Value parse_value() {
        skip_ws();
        char c = peek();
        if (c == '"') return parse_string();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        error(std::string("unexpected character '") + c + "'");
    }

    Value parse_string() {
        expect('"');
        std::string result;
        while (true) {
            if (pos_ >= input_.size()) error("unterminated string");
            char c = advance();
            if (c == '"') break;
            if (c == '\\') {
                if (pos_ >= input_.size()) error("unterminated escape");
                char e = advance();
                switch (e) {
                    case '"':  result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/':  result += '/'; break;
                    case 'b':  result += '\b'; break;
                    case 'f':  result += '\f'; break;
                    case 'n':  result += '\n'; break;
                    case 'r':  result += '\r'; break;
                    case 't':  result += '\t'; break;
                    case 'u': {
                        if (pos_ + 4 > input_.size()) error("incomplete unicode escape");
                        std::string hex(input_.substr(pos_, 4));
                        pos_ += 4;
                        auto cp = static_cast<char32_t>(std::stoul(hex, nullptr, 16));
                        if (cp < 0x80) {
                            result += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: result += e; break;
                }
            } else {
                result += c;
            }
        }
        return Value(std::move(result));
    }

    Value parse_number() {
        std::size_t start = pos_;
        if (peek() == '-') advance();
        while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') advance();
        bool is_float = false;
        if (peek() == '.') {
            is_float = true;
            advance();
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            advance();
            if (peek() == '+' || peek() == '-') advance();
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') advance();
        }
        std::string num_str(input_.substr(start, pos_ - start));
        if (is_float) return Value(std::stod(num_str));
        return Value(static_cast<int64_t>(std::stoll(num_str)));
    }

    Value parse_bool() {
        if (input_.substr(pos_, 4) == "true") { pos_ += 4; return Value(true); }
        if (input_.substr(pos_, 5) == "false") { pos_ += 5; return Value(false); }
        error("invalid boolean");
    }

    Value parse_null() {
        if (input_.substr(pos_, 4) == "null") { pos_ += 4; return Value(nullptr); }
        error("invalid null");
    }

    Value parse_array() {
        expect('[');
        Array arr;
        skip_ws();
        if (peek() == ']') { advance(); return Value(std::move(arr)); }
        while (true) {
            arr.push_back(parse_value());
            skip_ws();
            if (peek() == ']') { advance(); break; }
            expect(',');
        }
        return Value(std::move(arr));
    }

    Value parse_object() {
        expect('{');
        Object obj;
        skip_ws();
        if (peek() == '}') { advance(); return Value(std::move(obj)); }
        while (true) {
            skip_ws();
            auto key = parse_string();
            expect(':');
            auto val = parse_value();
            obj[key.as_string()] = std::move(val);
            skip_ws();
            if (peek() == '}') { advance(); break; }
            expect(',');
        }
        return Value(std::move(obj));
    }
};

inline Value parse(std::string_view input) {
    return Parser(input).parse();
}


inline Value object(std::initializer_list<std::pair<std::string, Value>> pairs) {
    Object o;
    for (auto& [k, v] : pairs) o.insert_or_assign(k, v);
    return Value(std::move(o));
}

inline Value array(std::initializer_list<Value> elems) {
    return Value(Array(elems));
}

} ///< namespace eta::json

