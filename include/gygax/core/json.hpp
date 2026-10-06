#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace gygax::json {

inline constexpr int kDefaultMaximumNestingDepth = 64;

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value, std::less<>>;

class Value {
public:
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : data_(v) {}
    Value(double v) : data_(v) {}
    Value(const char* v) : data_(std::string(v)) {}
    Value(std::string v) : data_(std::move(v)) {}
    Value(std::string_view v) : data_(std::string(v)) {}
    Value(Array v) : data_(std::move(v)) {}
    Value(Object v) : data_(std::move(v)) {}

    template <typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
    Value(T v) : data_(static_cast<std::int64_t>(v)) {}

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    [[nodiscard]] bool isNull() const { return std::holds_alternative<std::nullptr_t>(data_); }
    [[nodiscard]] bool isBool() const { return std::holds_alternative<bool>(data_); }
    [[nodiscard]] bool isInt() const { return std::holds_alternative<std::int64_t>(data_); }
    [[nodiscard]] bool isNumber() const { return isInt() || std::holds_alternative<double>(data_); }
    [[nodiscard]] bool isString() const { return std::holds_alternative<std::string>(data_); }
    [[nodiscard]] bool isArray() const { return std::holds_alternative<Array>(data_); }
    [[nodiscard]] bool isObject() const { return std::holds_alternative<Object>(data_); }

    [[nodiscard]] bool asBool(bool fallback = false) const {
        if (const auto* v = std::get_if<bool>(&data_)) return *v;
        return fallback;
    }

    [[nodiscard]] std::int64_t asInt(std::int64_t fallback = 0) const {
        if (const auto* v = std::get_if<std::int64_t>(&data_)) return *v;
        if (const auto* d = std::get_if<double>(&data_)) {
            if (std::isfinite(*d) && *d >= -9.2e18 && *d <= 9.2e18) return static_cast<std::int64_t>(*d);
        }
        return fallback;
    }

    [[nodiscard]] double asDouble(double fallback = 0.0) const {
        if (const auto* d = std::get_if<double>(&data_)) return *d;
        if (const auto* v = std::get_if<std::int64_t>(&data_)) return static_cast<double>(*v);
        return fallback;
    }

    [[nodiscard]] const std::string& asString() const {
        static const std::string empty;
        if (const auto* v = std::get_if<std::string>(&data_)) return *v;
        return empty;
    }

    [[nodiscard]] const Array& asArray() const {
        static const Array empty;
        if (const auto* v = std::get_if<Array>(&data_)) return *v;
        return empty;
    }

    [[nodiscard]] const Object& asObject() const {
        static const Object empty;
        if (const auto* v = std::get_if<Object>(&data_)) return *v;
        return empty;
    }

    Array& array_ref() {
        if (!isArray()) data_ = Array{};
        return std::get<Array>(data_);
    }

    Object& object_ref() {
        if (!isObject()) data_ = Object{};
        return std::get<Object>(data_);
    }

    Value& operator[](std::string_view key) {
        auto& obj = object_ref();
        auto it = obj.find(key);
        if (it == obj.end()) it = obj.emplace(std::string(key), Value{}).first;
        return it->second;
    }

    [[nodiscard]] const Value* find(std::string_view key) const {
        const auto* obj = std::get_if<Object>(&data_);
        if (obj == nullptr) return nullptr;
        auto it = obj->find(key);
        return it == obj->end() ? nullptr : &it->second;
    }

    [[nodiscard]] bool contains(std::string_view key) const { return find(key) != nullptr; }

    [[nodiscard]] std::string getString(std::string_view key, const std::string& fallback = {}) const {
        const auto* v = find(key);
        return (v != nullptr && v->isString()) ? v->asString() : fallback;
    }

    [[nodiscard]] std::int64_t getInt(std::string_view key, std::int64_t fallback = 0) const {
        const auto* v = find(key);
        return (v != nullptr && v->isNumber()) ? v->asInt(fallback) : fallback;
    }

    [[nodiscard]] double getDouble(std::string_view key, double fallback = 0.0) const {
        const auto* v = find(key);
        return (v != nullptr && v->isNumber()) ? v->asDouble(fallback) : fallback;
    }

    [[nodiscard]] bool getBool(std::string_view key, bool fallback = false) const {
        const auto* v = find(key);
        return (v != nullptr && v->isBool()) ? v->asBool(fallback) : fallback;
    }

    void push(Value v) { array_ref().push_back(std::move(v)); }

    [[nodiscard]] std::size_t size() const {
        if (const auto* a = std::get_if<Array>(&data_)) return a->size();
        if (const auto* o = std::get_if<Object>(&data_)) return o->size();
        return 0;
    }

    [[nodiscard]] std::string dump(int indent = -1) const {
        std::string out;
        write(out, indent, 0);
        return out;
    }

    friend bool operator==(const Value& a, const Value& b) { return a.data_ == b.data_; }

private:
    static void escape(std::string& out, std::string_view s) {
        out.push_back('"');
        for (const char raw : s) {
            const auto c = static_cast<unsigned char>(raw);
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(raw);
                }
            }
        }
        out.push_back('"');
    }

    static void newline(std::string& out, int indent, int depth) {
        if (indent < 0) return;
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth), ' ');
    }

    void write(std::string& out, int indent, int depth) const {
        if (isNull()) {
            out += "null";
        } else if (const auto* b = std::get_if<bool>(&data_)) {
            out += *b ? "true" : "false";
        } else if (const auto* i = std::get_if<std::int64_t>(&data_)) {
            out += std::to_string(*i);
        } else if (const auto* d = std::get_if<double>(&data_)) {
            if (!std::isfinite(*d)) {
                out += "null";
            } else {
                char buf[40];
                auto res = std::to_chars(buf, buf + sizeof(buf), *d);
                out.append(buf, res.ptr);
            }
        } else if (const auto* s = std::get_if<std::string>(&data_)) {
            escape(out, *s);
        } else if (const auto* a = std::get_if<Array>(&data_)) {
            if (a->empty()) {
                out += "[]";
                return;
            }
            out.push_back('[');
            bool first = true;
            for (const auto& item : *a) {
                if (!first) out.push_back(',');
                first = false;
                newline(out, indent, depth + 1);
                item.write(out, indent, depth + 1);
            }
            newline(out, indent, depth);
            out.push_back(']');
        } else if (const auto* o = std::get_if<Object>(&data_)) {
            if (o->empty()) {
                out += "{}";
                return;
            }
            out.push_back('{');
            bool first = true;
            for (const auto& [k, v] : *o) {
                if (!first) out.push_back(',');
                first = false;
                newline(out, indent, depth + 1);
                escape(out, k);
                out.push_back(':');
                if (indent >= 0) out.push_back(' ');
                v.write(out, indent, depth + 1);
            }
            newline(out, indent, depth);
            out.push_back('}');
        }
    }

    Storage data_;
};

class Parser {
public:
    Parser(std::string_view text, int maxDepth) : text_(text), maxDepth_(maxDepth) {}

    std::optional<Value> run(std::string* error) {
        skipSpace();
        auto value = parseValue(0);
        if (value) {
            skipSpace();
            if (pos_ != text_.size()) {
                fail("trailing characters after document");
                value.reset();
            }
        }
        if (!value && error != nullptr) *error = error_ + " at offset " + std::to_string(errorPos_);
        return value;
    }

private:
    void fail(std::string message) {
        if (error_.empty()) {
            error_ = std::move(message);
            errorPos_ = pos_;
        }
    }

    void skipSpace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume(std::string_view literal) {
        if (text_.substr(pos_, literal.size()) == literal) {
            pos_ += literal.size();
            return true;
        }
        return false;
    }

    std::optional<Value> parseValue(int depth) {
        if (depth > maxDepth_) {
            fail("maximum nesting depth exceeded");
            return std::nullopt;
        }
        if (pos_ >= text_.size()) {
            fail("unexpected end of input");
            return std::nullopt;
        }
        const char c = text_[pos_];
        if (c == '{') return parseObject(depth);
        if (c == '[') return parseArray(depth);
        if (c == '"') {
            auto s = parseString();
            if (!s) return std::nullopt;
            return Value(std::move(*s));
        }
        if (c == 't') return consume("true") ? std::optional<Value>(Value(true)) : bad();
        if (c == 'f') return consume("false") ? std::optional<Value>(Value(false)) : bad();
        if (c == 'n') return consume("null") ? std::optional<Value>(Value(nullptr)) : bad();
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        return bad();
    }

    std::optional<Value> bad() {
        fail("unexpected token");
        return std::nullopt;
    }

    std::optional<Value> parseNumber() {
        const std::size_t start = pos_;
        bool isFloat = false;
        if (text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size()) return bad();
        if (text_[pos_] == '0') {
            ++pos_;
        } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        } else {
            return bad();
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            isFloat = true;
            ++pos_;
            const std::size_t digits = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (pos_ == digits) return bad();
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            isFloat = true;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            const std::size_t digits = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (pos_ == digits) return bad();
        }
        const char* first = text_.data() + start;
        const char* last = text_.data() + pos_;
        if (!isFloat) {
            std::int64_t iv = 0;
            auto res = std::from_chars(first, last, iv);
            if (res.ec == std::errc() && res.ptr == last) return Value(iv);
        }
        double dv = 0.0;
        auto res = std::from_chars(first, last, dv);
        if (res.ec != std::errc() || res.ptr != last) {
            fail("invalid number");
            return std::nullopt;
        }
        return Value(dv);
    }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::optional<std::uint32_t> parseHex4() {
        if (pos_ + 4 > text_.size()) {
            fail("truncated unicode escape");
            return std::nullopt;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                fail("invalid unicode escape");
                return std::nullopt;
            }
        }
        return v;
    }

    std::optional<std::string> parseString() {
        ++pos_;
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("control character in string");
                return std::nullopt;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) break;
            const char e = text_[pos_++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                auto hi = parseHex4();
                if (!hi) return std::nullopt;
                std::uint32_t cp = *hi;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (!consume("\\u")) {
                        fail("unpaired surrogate");
                        return std::nullopt;
                    }
                    auto lo = parseHex4();
                    if (!lo) return std::nullopt;
                    if (*lo < 0xDC00 || *lo > 0xDFFF) {
                        fail("invalid low surrogate");
                        return std::nullopt;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (*lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    fail("unpaired surrogate");
                    return std::nullopt;
                }
                appendUtf8(out, cp);
                break;
            }
            default: fail("invalid escape sequence"); return std::nullopt;
            }
        }
        fail("unterminated string");
        return std::nullopt;
    }

    std::optional<Value> parseArray(int depth) {
        ++pos_;
        Array items;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return Value(std::move(items));
        }
        while (true) {
            skipSpace();
            auto item = parseValue(depth + 1);
            if (!item) return std::nullopt;
            items.push_back(std::move(*item));
            skipSpace();
            if (pos_ >= text_.size()) {
                fail("unterminated array");
                return std::nullopt;
            }
            const char c = text_[pos_++];
            if (c == ']') return Value(std::move(items));
            if (c != ',') {
                --pos_;
                fail("expected ',' or ']'");
                return std::nullopt;
            }
        }
    }

    std::optional<Value> parseObject(int depth) {
        ++pos_;
        Object members;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return Value(std::move(members));
        }
        while (true) {
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                fail("expected string key");
                return std::nullopt;
            }
            auto key = parseString();
            if (!key) return std::nullopt;
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                fail("expected ':'");
                return std::nullopt;
            }
            ++pos_;
            skipSpace();
            auto value = parseValue(depth + 1);
            if (!value) return std::nullopt;
            members[std::move(*key)] = std::move(*value);
            skipSpace();
            if (pos_ >= text_.size()) {
                fail("unterminated object");
                return std::nullopt;
            }
            const char c = text_[pos_++];
            if (c == '}') return Value(std::move(members));
            if (c != ',') {
                --pos_;
                fail("expected ',' or '}'");
                return std::nullopt;
            }
        }
    }

    std::string_view text_;
    int maxDepth_;
    std::size_t pos_ = 0;
    std::string error_;
    std::size_t errorPos_ = 0;
};

[[nodiscard]] inline std::optional<Value> parse(std::string_view text, std::string* error = nullptr,
                                                int maxDepth = kDefaultMaximumNestingDepth) {
    return Parser(text, maxDepth).run(error);
}

}
