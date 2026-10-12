#include <glyder/crypto.hpp>

#include <cstring>
#include <fcntl.h>
#include <random>
#include <unistd.h>

namespace glyder::crypto {

namespace {
constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
constexpr std::uint32_t rotr(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}
} // namespace

Sha256::Sha256() : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, buf_{} {}

void Sha256::block(const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t{p[4 * i]} << 24) | (std::uint32_t{p[4 * i + 1]} << 16) | (std::uint32_t{p[4 * i + 2]} << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data) {
    bytes_ += data.size();
    for (std::uint8_t byte : data) {
        buf_[used_++] = byte;
        if (used_ == 64) {
            block(buf_);
            used_ = 0;
        }
    }
}

Digest Sha256::finish() {
    const std::uint64_t bits = bytes_ * 8;
    const std::uint8_t one = 0x80, zero = 0;
    update({&one, 1});
    while (used_ != 56) update({&zero, 1});
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(len);
    Digest d;
    for (std::size_t i = 0; i < 8; ++i)
        for (std::size_t j = 0; j < 4; ++j) d[4 * i + j] = static_cast<std::uint8_t>(h_[i] >> (24 - 8 * j));
    return d;
}

Digest sha256(std::span<const std::uint8_t> data) {
    Sha256 s;
    s.update(data);
    return s.finish();
}

Digest hmac(std::span<const std::uint8_t> key, std::span<const std::uint8_t> message) {
    std::uint8_t k[64]{};
    if (key.size() > 64) {
        const Digest kd = sha256(key);
        std::memcpy(k, kd.data(), kd.size());
    } else if (!key.empty()) {
        std::memcpy(k, key.data(), key.size());
    }
    std::uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5c);
    }
    Sha256 inner;
    inner.update(ipad);
    inner.update(message);
    const Digest ih = inner.finish();
    Sha256 outer;
    outer.update(opad);
    outer.update(ih);
    return outer.finish();
}

bool equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    if (a.size() != b.size()) return false;
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff = static_cast<std::uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

void randomBytes(std::span<std::uint8_t> out) {
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    std::size_t got = 0;
    if (fd >= 0) {
        while (got < out.size()) {
            const ssize_t n = ::read(fd, out.data() + got, out.size() - got);
            if (n <= 0) break;
            got += static_cast<std::size_t>(n);
        }
        ::close(fd);
    }
    if (got < out.size()) {
        std::random_device rd;
        for (std::size_t i = got; i < out.size(); ++i) out[i] = static_cast<std::uint8_t>(rd());
    }
}

} // namespace glyder::crypto
