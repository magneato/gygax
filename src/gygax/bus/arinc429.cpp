#include <gygax/bus/arinc429.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace gygax::bus::arinc429 {

std::uint8_t labelFromOctal(unsigned octal) {
    const unsigned value = ((octal / 100) % 10) * 64 + ((octal / 10) % 10) * 8 + (octal % 10);
    std::uint8_t reversed = 0;
    for (int i = 0; i < 8; ++i) {
        if ((value >> i) & 1U) reversed = static_cast<std::uint8_t>(reversed | (1U << (7 - i)));
    }
    return reversed;
}

unsigned labelToOctal(std::uint8_t label) {
    unsigned value = 0;
    for (int i = 0; i < 8; ++i) {
        if ((label >> i) & 1U) value |= 1U << (7 - i);
    }
    return (value / 64) * 100 + ((value / 8) % 8) * 10 + (value % 8);
}

bool parityOk(std::uint32_t raw) {
    return std::popcount(raw) % 2 == 1;
}

std::uint32_t encode(const Word& w) {
    std::uint32_t raw = w.label;
    raw |= static_cast<std::uint32_t>(w.sdi & 0x3) << 8;
    raw |= (w.data & 0x7FFFF) << 10;
    raw |= static_cast<std::uint32_t>(w.ssm & 0x3) << 29;
    if (!parityOk(raw)) raw |= 0x80000000U;
    return raw;
}

std::optional<Word> decode(std::uint32_t raw, bool requireOddParity) {
    if (requireOddParity && !parityOk(raw)) return std::nullopt;
    Word w;
    w.label = static_cast<std::uint8_t>(raw & 0xFF);
    w.sdi = static_cast<std::uint8_t>((raw >> 8) & 0x3);
    w.data = (raw >> 10) & 0x7FFFF;
    w.ssm = static_cast<std::uint8_t>((raw >> 29) & 0x3);
    return w;
}

std::uint32_t encodeBnr(double value, unsigned bits, double fullScale) {
    if (bits < 2 || bits > 19) return 0;
    const double lsb = fullScale / std::ldexp(1.0, static_cast<int>(bits) - 1);
    const double limit = std::ldexp(1.0, static_cast<int>(bits) - 1) - 1.0;
    const double scaled = std::clamp(std::round(value / lsb), -limit - 1.0, limit);
    auto twos = static_cast<std::int64_t>(scaled);
    if (twos < 0) twos += std::int64_t{1} << bits;
    return static_cast<std::uint32_t>(twos) & ((1U << bits) - 1U);
}

double decodeBnr(std::uint32_t data, unsigned bits, double fullScale) {
    if (bits < 2 || bits > 19) return 0.0;
    std::int64_t v = data & ((1U << bits) - 1U);
    if ((v >> (bits - 1)) != 0) v -= std::int64_t{1} << bits;
    return static_cast<double>(v) * fullScale / std::ldexp(1.0, static_cast<int>(bits) - 1);
}

std::uint32_t encodeBcd(unsigned digits, unsigned value) {
    std::uint32_t out = 0;
    for (unsigned i = 0; i < digits; ++i) {
        out |= (value % 10) << (4 * i);
        value /= 10;
    }
    return out;
}

std::optional<unsigned> decodeBcd(std::uint32_t data, unsigned digits) {
    unsigned out = 0;
    unsigned scale = 1;
    for (unsigned i = 0; i < digits; ++i) {
        const unsigned nibble = (data >> (4 * i)) & 0xF;
        if (nibble > 9) return std::nullopt;
        out += nibble * scale;
        scale *= 10;
    }
    return out;
}

std::vector<std::uint32_t> readWords(std::span<const std::uint8_t> bytes, bool bigEndian) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        const auto* p = bytes.data() + i;
        out.push_back(bigEndian ? (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                                      (static_cast<std::uint32_t>(p[2]) << 8) | p[3]
                                : (static_cast<std::uint32_t>(p[3]) << 24) | (static_cast<std::uint32_t>(p[2]) << 16) |
                                      (static_cast<std::uint32_t>(p[1]) << 8) | p[0]);
    }
    return out;
}

} // namespace gygax::bus::arinc429
