#include <glyder/image.hpp>

#include <array>
#include <fstream>

namespace glyder::image {

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) {
    static const auto table = [] {
        std::uint32_t t[256];
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return std::to_array(t);
    }();
    crc = ~crc;
    for (std::uint8_t b : data) crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

namespace {

void be32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    o.push_back(static_cast<std::uint8_t>(v >> 24));
    o.push_back(static_cast<std::uint8_t>(v >> 16));
    o.push_back(static_cast<std::uint8_t>(v >> 8));
    o.push_back(static_cast<std::uint8_t>(v));
}

void chunk(std::vector<std::uint8_t>& o, const char* type, const std::vector<std::uint8_t>& data) {
    be32(o, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    o.insert(o.end(), body.begin(), body.end());
    be32(o, crc32(body));
}

} // namespace

std::vector<std::uint8_t> encodePng(int width, int height, std::span<const std::uint8_t> rgba) {
    const std::size_t w = static_cast<std::size_t>(width), h = static_cast<std::size_t>(height);
    std::vector<std::uint8_t> raw;
    raw.reserve(h * (w * 4 + 1));
    for (std::size_t y = 0; y < h; ++y) {
        raw.push_back(0); // filter: none
        raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(y * w * 4),
                   rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * w * 4));
    }
    // zlib stream of stored (uncompressed) deflate blocks.
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t v : raw) {
        a = (a + v) % 65521u;
        b = (b + a) % 65521u;
    }
    for (std::size_t off = 0; off < raw.size() || off == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off), raw.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
        if (last) break;
    }
    be32(z, (b << 16) | a);

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, static_cast<std::uint32_t>(width));
    be32(ihdr, static_cast<std::uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

bool writePng(const std::string& path, int width, int height, std::span<const std::uint8_t> rgba) {
    const auto png = encodePng(width, height, rgba);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(f);
}

} // namespace glyder::image
