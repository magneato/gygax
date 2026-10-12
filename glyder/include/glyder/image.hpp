#pragma once

// Minimal image output: RGBA8 to PNG with no dependencies (stored deflate blocks), for headless
// captures, the kit gallery and tests.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace glyder::image {

[[nodiscard]] std::vector<std::uint8_t> encodePng(int width, int height, std::span<const std::uint8_t> rgba);
bool writePng(const std::string& path, int width, int height, std::span<const std::uint8_t> rgba);
[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0);

} // namespace glyder::image
