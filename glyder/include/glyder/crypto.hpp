#pragma once

// SHA-256 and HMAC-SHA-256 (FIPS 180-4, RFC 2104), for authenticating glyder sessions without a
// crypto dependency. Constant-time comparison for tags.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace glyder::crypto {

using Digest = std::array<std::uint8_t, 32>;

class Sha256 {
public:
    Sha256();
    void update(std::span<const std::uint8_t> data);
    void update(std::string_view s) { update({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}); }
    [[nodiscard]] Digest finish();

private:
    void block(const std::uint8_t* p);
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::uint64_t bytes_{0};
    std::size_t used_{0};
};

[[nodiscard]] Digest sha256(std::span<const std::uint8_t> data);
[[nodiscard]] Digest hmac(std::span<const std::uint8_t> key, std::span<const std::uint8_t> message);
[[nodiscard]] bool equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b);
// Fill with unpredictable bytes from the OS.
void randomBytes(std::span<std::uint8_t> out);

} // namespace glyder::crypto
