#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace gygax::bus::arinc429 {

struct Word {
    std::uint8_t label = 0;
    std::uint8_t sdi = 0;
    std::uint32_t data = 0;
    std::uint8_t ssm = 0;
};

std::uint32_t encode(const Word& word);
std::optional<Word> decode(std::uint32_t raw, bool requireOddParity = true);
bool parityOk(std::uint32_t raw);
std::uint8_t labelFromOctal(unsigned octal);
unsigned labelToOctal(std::uint8_t label);

std::uint32_t encodeBnr(double value, unsigned bits, double fullScale);
double decodeBnr(std::uint32_t data, unsigned bits, double fullScale);
std::uint32_t encodeBcd(unsigned digits, unsigned value);
std::optional<unsigned> decodeBcd(std::uint32_t data, unsigned digits);

std::vector<std::uint32_t> readWords(std::span<const std::uint8_t> bytes, bool bigEndian);

} // namespace gygax::bus::arinc429
