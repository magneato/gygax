#include <gygax/intel/metadata.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>

namespace gygax::intel {

namespace {

constexpr std::size_t kMaxIfdEntries = 512;
constexpr int kMaxIfdDepth = 4;
constexpr std::uint32_t kMaxValueBytes = 1U << 20;
constexpr std::uint32_t kGpsSecondsDenominator = 10000;

constexpr std::uint16_t kTagImageDescription = 0x010E;
constexpr std::uint16_t kTagMake = 0x010F;
constexpr std::uint16_t kTagModel = 0x0110;
constexpr std::uint16_t kTagSoftware = 0x0131;
constexpr std::uint16_t kTagDateTime = 0x0132;
constexpr std::uint16_t kTagExifIfd = 0x8769;
constexpr std::uint16_t kTagGpsIfd = 0x8825;
constexpr std::uint16_t kTagDateTimeOriginal = 0x9003;

constexpr std::uint16_t kGpsLatRef = 1;
constexpr std::uint16_t kGpsLat = 2;
constexpr std::uint16_t kGpsLonRef = 3;
constexpr std::uint16_t kGpsLon = 4;
constexpr std::uint16_t kGpsAltRef = 5;
constexpr std::uint16_t kGpsAlt = 6;
constexpr std::uint16_t kGpsImgDirection = 17;

enum class Ifd { Main, Exif, Gps };

enum Type : std::uint16_t {
    kByte = 1,
    kAscii = 2,
    kShort = 3,
    kLong = 4,
    kRational = 5,
    kUndefined = 7,
    kSLong = 9,
    kSRational = 10,
};

std::size_t typeSize(std::uint16_t type) {
    switch (type) {
    case kByte:
    case kAscii:
    case kUndefined:
    case 6: return 1;
    case kShort:
    case 8: return 2;
    case kLong:
    case kSLong:
    case 11: return 4;
    case kRational:
    case kSRational:
    case 12: return 8;
    default: return 0;
    }
}

struct Reader {
    std::span<const std::uint8_t> data;
    bool little = true;

    [[nodiscard]] std::optional<std::uint16_t> u16(std::size_t o) const {
        if (o > data.size() || data.size() - o < 2) return std::nullopt;
        return little ? static_cast<std::uint16_t>(data[o] | (data[o + 1] << 8))
                      : static_cast<std::uint16_t>((data[o] << 8) | data[o + 1]);
    }
    [[nodiscard]] std::optional<std::uint32_t> u32(std::size_t o) const {
        if (o > data.size() || data.size() - o < 4) return std::nullopt;
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const std::uint32_t b = data[o + (little ? 3 - i : i)];
            v = (v << 8) | b;
        }
        return v;
    }
};

struct Entry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::size_t valueOffset = 0;
};

std::optional<Entry> readEntry(const Reader& r, std::size_t at) {
    const auto tag = r.u16(at);
    const auto type = r.u16(at + 2);
    const auto count = r.u32(at + 4);
    if (!tag || !type || !count) return std::nullopt;
    const std::size_t unit = typeSize(*type);
    if (unit == 0) return std::nullopt;
    const std::uint64_t total = static_cast<std::uint64_t>(unit) * *count;
    if (total > kMaxValueBytes) return std::nullopt;
    std::size_t where = at + 8;
    if (total > 4) {
        const auto off = r.u32(at + 8);
        if (!off) return std::nullopt;
        where = *off;
    }
    if (where > r.data.size() || r.data.size() - where < total) return std::nullopt;
    return Entry{*tag, *type, *count, where};
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && s[start] == ' ') ++start;
    return s.substr(start);
}

std::string readAscii(const Reader& r, const Entry& e) {
    std::string s(reinterpret_cast<const char*>(r.data.data() + e.valueOffset), e.count);
    if (const auto nul = s.find('\0'); nul != std::string::npos) s.resize(nul);
    return trim(std::move(s));
}

std::optional<double> readRational(const Reader& r, const Entry& e, std::size_t index) {
    if (index >= e.count || (e.type != kRational && e.type != kSRational)) return std::nullopt;
    const auto num = r.u32(e.valueOffset + index * 8);
    const auto den = r.u32(e.valueOffset + index * 8 + 4);
    if (!num || !den || *den == 0) return std::nullopt;
    if (e.type == kSRational) return static_cast<double>(static_cast<std::int32_t>(*num)) / static_cast<double>(static_cast<std::int32_t>(*den));
    return static_cast<double>(*num) / static_cast<double>(*den);
}

std::optional<double> readDegrees(const Reader& r, const Entry& e) {
    if (e.count < 3) return std::nullopt;
    const auto d = readRational(r, e, 0);
    const auto m = readRational(r, e, 1);
    const auto s = readRational(r, e, 2);
    if (!d || !m || !s) return std::nullopt;
    return *d + *m / 60.0 + *s / 3600.0;
}

struct Gps {
    char latRef = 'N';
    char lonRef = 'E';
    std::optional<double> lat;
    std::optional<double> lon;
    std::optional<double> alt;
    bool below = false;
    std::optional<double> heading;
};

char firstChar(const Reader& r, const Entry& e) {
    return e.count > 0 ? static_cast<char>(r.data[e.valueOffset]) : '\0';
}

void walkIfd(const Reader& r, std::size_t offset, Ifd which, int depth, ImageMetadata& out, Gps& gps) {
    if (depth > kMaxIfdDepth) return;
    const auto n = r.u16(offset);
    if (!n) return;
    const std::size_t available = (r.data.size() - offset - 2) / 12;
    const std::size_t count = std::min<std::size_t>({*n, available, kMaxIfdEntries});
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = offset + 2 + i * 12;
        const auto e = readEntry(r, at);
        if (!e) continue;
        if (which == Ifd::Gps) {
            switch (e->tag) {
            case kGpsLatRef: gps.latRef = firstChar(r, *e); break;
            case kGpsLonRef: gps.lonRef = firstChar(r, *e); break;
            case kGpsLat: gps.lat = readDegrees(r, *e); break;
            case kGpsLon: gps.lon = readDegrees(r, *e); break;
            case kGpsAltRef: gps.below = e->count > 0 && r.data[e->valueOffset] == 1; break;
            case kGpsAlt: gps.alt = readRational(r, *e, 0); break;
            case kGpsImgDirection: gps.heading = readRational(r, *e, 0); break;
            default: break;
            }
            continue;
        }
        switch (e->tag) {
        case kTagImageDescription: out.description = readAscii(r, *e); break;
        case kTagMake: out.make = readAscii(r, *e); break;
        case kTagModel: out.model = readAscii(r, *e); break;
        case kTagSoftware: out.software = readAscii(r, *e); break;
        case kTagDateTime:
            if (out.dateTime.empty()) out.dateTime = readAscii(r, *e);
            break;
        case kTagDateTimeOriginal: out.dateTime = readAscii(r, *e); break;
        case kTagExifIfd:
        case kTagGpsIfd:
            if (e->type == kLong && e->count == 1) {
                if (const auto sub = r.u32(e->valueOffset)) {
                    walkIfd(r, *sub, e->tag == kTagGpsIfd ? Ifd::Gps : Ifd::Exif, depth + 1, out, gps);
                }
            }
            break;
        default: break;
        }
    }
}

bool inRange(double v, double limit) {
    return std::isfinite(v) && std::abs(v) <= limit;
}

// IPTC-IIM: records of 0x1C, record number, dataset number and a two-byte length.
void parseIptc(std::span<const std::uint8_t> d, ImageMetadata& out) {
    std::size_t p = 0;
    while (d.size() - p >= 5 && d[p] == 0x1C) {
        const std::uint8_t record = d[p + 1];
        const std::uint8_t dataset = d[p + 2];
        const std::size_t len = static_cast<std::size_t>((d[p + 3] << 8) | d[p + 4]);
        if ((len & 0x8000U) != 0 || d.size() - (p + 5) < len) return;
        const std::string value = trim(std::string(reinterpret_cast<const char*>(d.data() + p + 5), len));
        if (record == 2) {
            switch (dataset) {
            case 5: out.iptcTitle = value; break;
            case 25: out.iptcKeywords.push_back(value); break;
            case 90: out.iptcCity = value; break;
            case 101: out.iptcCountry = value; break;
            case 120: out.iptcCaption = value; break;
            default: break;
            }
        }
        p += 5 + len;
    }
}

// Photoshop image resources (APP13): 8BIM blocks, resource 0x0404 holds the IPTC stream.
void parsePhotoshop(std::span<const std::uint8_t> d, ImageMetadata& out) {
    constexpr std::string_view kSig = "Photoshop 3.0";
    if (d.size() < kSig.size() + 1 || std::memcmp(d.data(), kSig.data(), kSig.size()) != 0) return;
    std::size_t p = kSig.size() + 1;
    while (d.size() - p >= 12 && std::memcmp(d.data() + p, "8BIM", 4) == 0) {
        const std::uint16_t id = static_cast<std::uint16_t>((d[p + 4] << 8) | d[p + 5]);
        std::size_t q = p + 6;
        const std::size_t nameLen = d[q];
        q += 1 + nameLen;
        if ((1 + nameLen) % 2 != 0) ++q;
        if (q > d.size() || d.size() - q < 4) return;
        const std::size_t size = (static_cast<std::size_t>(d[q]) << 24) | (static_cast<std::size_t>(d[q + 1]) << 16) |
                                 (static_cast<std::size_t>(d[q + 2]) << 8) | d[q + 3];
        q += 4;
        if (d.size() - q < size) return;
        if (id == 0x0404) parseIptc(d.subspan(q, size), out);
        p = q + size + (size % 2);
        if (p > d.size()) return;
    }
}

} // namespace

bool parseTiff(std::span<const std::uint8_t> data, ImageMetadata& out) {
    if (data.size() < 8) return false;
    Reader r{data, true};
    if (data[0] == 'I' && data[1] == 'I') {
        r.little = true;
    } else if (data[0] == 'M' && data[1] == 'M') {
        r.little = false;
    } else {
        return false;
    }
    if (r.u16(2) != 42) return false;
    const auto ifd0 = r.u32(4);
    if (!ifd0) return false;
    if (out.format.empty()) out.format = "tiff";
    Gps gps;
    walkIfd(r, *ifd0, Ifd::Main, 0, out, gps);
    if (gps.lat && gps.lon) {
        const double lat = gps.latRef == 'S' ? -*gps.lat : *gps.lat;
        const double lon = gps.lonRef == 'W' ? -*gps.lon : *gps.lon;
        if (inRange(lat, 90.0) && inRange(lon, 180.0)) {
            out.latitude = lat;
            out.longitude = lon;
        }
    }
    if (gps.alt) out.altitude = gps.below ? -*gps.alt : *gps.alt;
    if (gps.heading) out.heading = *gps.heading;
    return true;
}

bool parseJpeg(std::span<const std::uint8_t> data, ImageMetadata& out) {
    if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    out.format = "jpeg";
    std::size_t p = 2;
    while (p + 4 <= data.size()) {
        if (data[p] != 0xFF) return true;
        while (p < data.size() && data[p] == 0xFF) ++p;
        if (p >= data.size()) return true;
        const std::uint8_t marker = data[p++];
        if (marker == 0xD9 || marker == 0xDA) return true;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (data.size() - p < 2) return true;
        const std::size_t len = static_cast<std::size_t>((data[p] << 8) | data[p + 1]);
        if (len < 2 || data.size() - p < len) return true;
        const auto body = data.subspan(p + 2, len - 2);
        if (marker == 0xE1 && body.size() > 6 && std::memcmp(body.data(), "Exif\0\0", 6) == 0) {
            parseTiff(body.subspan(6), out);
            out.format = "jpeg";
        } else if (marker == 0xED) {
            parsePhotoshop(body, out);
        }
        p += len;
    }
    return true;
}

bool parsePng(std::span<const std::uint8_t> data, ImageMetadata& out) {
    static constexpr std::array<std::uint8_t, 8> kSig{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (data.size() < kSig.size() || !std::equal(kSig.begin(), kSig.end(), data.begin())) return false;
    out.format = "png";
    std::size_t p = kSig.size();
    while (data.size() - p >= 12) {
        const std::size_t len = (static_cast<std::size_t>(data[p]) << 24) | (static_cast<std::size_t>(data[p + 1]) << 16) |
                                (static_cast<std::size_t>(data[p + 2]) << 8) | data[p + 3];
        if (data.size() - p - 12 < len) return true;
        if (std::memcmp(data.data() + p + 4, "eXIf", 4) == 0) {
            parseTiff(data.subspan(p + 8, len), out);
            out.format = "png";
        } else if (std::memcmp(data.data() + p + 4, "IEND", 4) == 0) {
            return true;
        }
        p += 12 + len;
    }
    return true;
}

bool parseImage(std::span<const std::uint8_t> data, ImageMetadata& out) {
    return parseJpeg(data, out) || parsePng(data, out) || parseTiff(data, out);
}

bool parseImageFile(const std::string& path, ImageMetadata& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) *error = "cannot open " + path;
        return false;
    }
    constexpr std::size_t kMaxImageBytes = 256U << 20;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() > kMaxImageBytes) {
        if (error != nullptr) *error = "file too large";
        return false;
    }
    if (!parseImage(bytes, out)) {
        if (error != nullptr) *error = "not a JPEG, PNG or TIFF image";
        return false;
    }
    return true;
}

namespace {

struct Writer {
    std::vector<std::uint8_t> bytes;
    void u16(std::uint16_t v) {
        bytes.push_back(static_cast<std::uint8_t>(v & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
};

struct OutEntry {
    std::uint16_t tag;
    std::uint16_t type;
    std::uint32_t count;
    std::vector<std::uint8_t> value; // little-endian payload
    bool pointer = false;            // value is the offset of the GPS IFD, filled in at layout
};

std::vector<std::uint8_t> asciiBytes(const std::string& s) {
    std::vector<std::uint8_t> v(s.begin(), s.end());
    v.push_back(0);
    return v;
}

OutEntry ascii(std::uint16_t tag, const std::string& s) {
    auto v = asciiBytes(s);
    return {tag, kAscii, static_cast<std::uint32_t>(v.size()), std::move(v)};
}

void putU32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
}

OutEntry degrees(std::uint16_t tag, double value) {
    const auto total = static_cast<std::uint64_t>(std::llround(std::abs(value) * 3600.0 * kGpsSecondsDenominator));
    const std::uint64_t perDegree = 3600ULL * kGpsSecondsDenominator;
    const std::uint64_t perMinute = 60ULL * kGpsSecondsDenominator;
    OutEntry e{tag, kRational, 3, {}};
    putU32(e.value, static_cast<std::uint32_t>(total / perDegree));
    putU32(e.value, 1);
    putU32(e.value, static_cast<std::uint32_t>(total % perDegree / perMinute));
    putU32(e.value, 1);
    putU32(e.value, static_cast<std::uint32_t>(total % perMinute));
    putU32(e.value, kGpsSecondsDenominator);
    return e;
}

OutEntry rational(std::uint16_t tag, double value) {
    OutEntry e{tag, kRational, 1, {}};
    putU32(e.value, static_cast<std::uint32_t>(std::llround(std::abs(value) * 1000.0)));
    putU32(e.value, 1000);
    return e;
}

OutEntry refChar(std::uint16_t tag, char c) {
    return {tag, kAscii, 2, {static_cast<std::uint8_t>(c), 0}};
}

std::size_t ifdSize(std::size_t entries) {
    return 2 + entries * 12 + 4;
}

} // namespace

std::vector<std::uint8_t> buildTiff(const ImageMetadata& meta) {
    std::vector<OutEntry> main;
    if (!meta.description.empty()) main.push_back(ascii(kTagImageDescription, meta.description));
    if (!meta.make.empty()) main.push_back(ascii(kTagMake, meta.make));
    if (!meta.model.empty()) main.push_back(ascii(kTagModel, meta.model));
    if (!meta.software.empty()) main.push_back(ascii(kTagSoftware, meta.software));
    if (!meta.dateTime.empty()) main.push_back(ascii(kTagDateTime, meta.dateTime));

    std::vector<OutEntry> gps;
    if (meta.hasGps()) {
        gps.push_back(refChar(kGpsLatRef, *meta.latitude >= 0 ? 'N' : 'S'));
        gps.push_back(degrees(kGpsLat, *meta.latitude));
        gps.push_back(refChar(kGpsLonRef, *meta.longitude >= 0 ? 'E' : 'W'));
        gps.push_back(degrees(kGpsLon, *meta.longitude));
        if (meta.altitude) {
            gps.push_back({kGpsAltRef, kByte, 1, {static_cast<std::uint8_t>(*meta.altitude < 0 ? 1 : 0)}});
            gps.push_back(rational(kGpsAlt, *meta.altitude));
        }
        if (meta.heading) gps.push_back(rational(kGpsImgDirection, *meta.heading));
        OutEntry ptr{kTagGpsIfd, kLong, 1, {}};
        ptr.pointer = true;
        main.push_back(std::move(ptr));
    }
    std::sort(main.begin(), main.end(), [](const OutEntry& a, const OutEntry& b) { return a.tag < b.tag; });

    const std::size_t mainStart = 8;
    const std::size_t gpsStart = mainStart + ifdSize(main.size());
    const std::size_t blobStart = gps.empty() ? gpsStart : gpsStart + ifdSize(gps.size());

    std::vector<std::uint8_t> blob;
    Writer w;
    w.bytes = {'I', 'I', 0x2A, 0x00};
    w.u32(static_cast<std::uint32_t>(mainStart));

    auto emit = [&](const std::vector<OutEntry>& entries) {
        w.u16(static_cast<std::uint16_t>(entries.size()));
        for (const auto& e : entries) {
            w.u16(e.tag);
            w.u16(e.type);
            w.u32(e.count);
            if (e.pointer) {
                w.u32(static_cast<std::uint32_t>(gpsStart));
            } else if (e.value.size() <= 4) {
                auto padded = e.value;
                padded.resize(4, 0);
                w.bytes.insert(w.bytes.end(), padded.begin(), padded.end());
            } else {
                w.u32(static_cast<std::uint32_t>(blobStart + blob.size()));
                blob.insert(blob.end(), e.value.begin(), e.value.end());
                if (blob.size() % 2 != 0) blob.push_back(0);
            }
        }
        w.u32(0);
    };
    emit(main);
    if (!gps.empty()) emit(gps);
    w.bytes.insert(w.bytes.end(), blob.begin(), blob.end());
    return std::move(w.bytes);
}

std::vector<std::uint8_t> buildJpeg(const ImageMetadata& meta) {
    const auto tiff = buildTiff(meta);
    const std::size_t segment = 2 + 6 + tiff.size();
    std::vector<std::uint8_t> out{0xFF, 0xD8, 0xFF, 0xE1, static_cast<std::uint8_t>(segment >> 8), static_cast<std::uint8_t>(segment & 0xFF),
                                  'E',  'x',  'i',  'f',  0,                                       0};
    out.insert(out.end(), tiff.begin(), tiff.end());
    out.push_back(0xFF);
    out.push_back(0xD9);
    return out;
}

} // namespace gygax::intel
