#pragma once

#include <charconv>
#include <system_error>
#include <type_traits>

#include <cctype>
#include <cerrno>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <string>

namespace gygax {

namespace detail {

// Reads the same text std::from_chars accepts for floating point in the default (general)
// format: optional '-', then digits with optional fraction and exponent, or inf, infinity or
// nan (any case, nan optionally followed by "(chars)"). No leading '+', spaces or hex. Used
// where std::from_chars for floating point is missing.
template <class T> std::from_chars_result parseFloating(const char* first, const char* last, T& value) {
    static_assert(std::is_floating_point_v<T>);
    const char* p = first;
    auto digits = [&] {
        const char* start = p;
        while (p < last && *p >= '0' && *p <= '9') ++p;
        return p != start;
    };
    if (p < last && *p == '-') ++p;
    auto word = [&](const char* w) {
        const char* q = p;
        for (; *w != '\0'; ++w, ++q)
            if (q >= last || (*q | 0x20) != *w) return false;
        p = q;
        return true;
    };
    if (word("infinity") || word("inf")) {
        value = first[0] == '-' ? -std::numeric_limits<T>::infinity() : std::numeric_limits<T>::infinity();
        return {p, std::errc()};
    }
    if (word("nan")) {
        if (p < last && *p == '(') {
            const char* q = p + 1;
            while (q < last && (std::isalnum(static_cast<unsigned char>(*q)) != 0 || *q == '_')) ++q;
            if (q < last && *q == ')') p = q + 1;
        }
        value = first[0] == '-' ? -std::numeric_limits<T>::quiet_NaN() : std::numeric_limits<T>::quiet_NaN();
        return {p, std::errc()};
    }
    bool any = digits();
    if (p < last && *p == '.') {
        ++p;
        any = digits() || any;
    }
    if (!any) return {first, std::errc::invalid_argument};
    if (p < last && (*p == 'e' || *p == 'E')) {
        const char* mark = p;
        ++p;
        if (p < last && (*p == '+' || *p == '-')) ++p;
        if (!digits()) p = mark;
    }
    const std::string token(first, p);
    char* end = nullptr;
    errno = 0;
    T parsed{};
    if constexpr (std::is_same_v<T, float>)
        parsed = std::strtof(token.c_str(), &end);
    else if constexpr (std::is_same_v<T, double>)
        parsed = std::strtod(token.c_str(), &end);
    else
        parsed = std::strtold(token.c_str(), &end);
    if (end != token.c_str() + token.size()) return {first, std::errc::invalid_argument};
    // strtod also reports ERANGE for subnormal results; std::from_chars only fails when the
    // value overflows to infinity or underflows all the way to zero.
    if (errno == ERANGE && (std::isinf(parsed) || parsed == 0)) return {p, std::errc::result_out_of_range};
    value = parsed;
    return {p, std::errc()};
}

} // namespace detail

// std::from_chars, except that Apple's libc++ marks the floating-point overloads unavailable
// before macOS 26; there floating point goes through detail::parseFloating, which accepts
// exactly the same text. Integers always use std::from_chars.
template <class T, class... Base> std::from_chars_result fromChars(const char* first, const char* last, T& value, Base... base) {
#if defined(__APPLE__)
    if constexpr (std::is_floating_point_v<T>) {
        static_assert(sizeof...(Base) == 0, "fromChars: chars_format is not supported for floating point");
        return detail::parseFloating(first, last, value);
    } else {
        return std::from_chars(first, last, value, base...);
    }
#else
    return std::from_chars(first, last, value, base...);
#endif
}

} // namespace gygax
