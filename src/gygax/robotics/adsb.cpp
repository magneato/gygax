#include <gygax/robotics/adsb.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <sstream>

namespace gygax::robotics::adsb {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kAdsbFrameSizeBytes = 14;
constexpr std::size_t kAdsbFrameSizeHexCharacters = 2 * kAdsbFrameSizeBytes;
constexpr int kAdsbAddressParityMessageBits = 88;
constexpr int kAdsbParityBits = 24;
constexpr int kAdsbCprLatitudeEvenZones = 60;
constexpr int kAdsbCprLatitudeOddZones = 59;
constexpr double kAdsbCprScale = 131072.0;
constexpr std::uint32_t kAdsbCrcPolynomial = 0x1FFF409;
constexpr std::uint32_t kAdsbCrcRegisterTopBitMask = 0x1000000U;
constexpr std::uint32_t kAdsbCrcParityMask = 0xFFFFFFU;
constexpr double kAdsbCprLatitudeTransitionDegrees = 87.0;
constexpr std::size_t kMaximumAvrBufferBytes = 256;

std::uint64_t bits(const Frame& f, int first, int count) {
    std::uint64_t v = 0;
    for (int i = 0; i < count; ++i) {
        const int bit = first + i;
        v = (v << 1) | ((f[static_cast<std::size_t>(bit / 8)] >> (7 - bit % 8)) & 1U);
    }
    return v;
}

double mod(double x, double y) {
    return x - y * std::floor(x / y);
}

constexpr std::string_view kCharset = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";

} // namespace

std::optional<Frame> parseHex(std::string_view hex) {
    while (!hex.empty() && (hex.front() == '*' || hex.front() == '@')) hex.remove_prefix(1);
    while (!hex.empty() && (hex.back() == ';' || hex.back() == '\r' || hex.back() == '\n' || hex.back() == ' ')) hex.remove_suffix(1);
    if (hex.size() != kAdsbFrameSizeHexCharacters) return std::nullopt;
    Frame f{};
    for (std::size_t i = 0; i < kAdsbFrameSizeBytes; ++i) {
        unsigned v = 0;
        if (std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, v, 16).ec != std::errc()) return std::nullopt;
        f[i] = static_cast<std::uint8_t>(v);
    }
    return f;
}

std::uint32_t crc24(const Frame& frame) {
    std::uint32_t reg = 0;
    for (int i = 0; i < kAdsbAddressParityMessageBits; ++i) {
        const std::uint32_t bit = (frame[static_cast<std::size_t>(i / 8)] >> (7 - i % 8)) & 1U;
        reg = (reg << 1) | bit;
        if ((reg & kAdsbCrcRegisterTopBitMask) != 0) reg ^= kAdsbCrcPolynomial;
    }
    for (int i = 0; i < kAdsbParityBits; ++i) {
        reg <<= 1;
        if ((reg & kAdsbCrcRegisterTopBitMask) != 0) reg ^= kAdsbCrcPolynomial;
    }
    return reg & kAdsbCrcParityMask;
}

bool crcValid(const Frame& frame) {
    const std::uint32_t parity = (static_cast<std::uint32_t>(frame[11]) << 16) | (static_cast<std::uint32_t>(frame[12]) << 8) | frame[13];
    return crc24(frame) == parity;
}

int downlinkFormat(const Frame& frame) {
    return frame[0] >> 3;
}

std::uint32_t icao(const Frame& frame) {
    return (static_cast<std::uint32_t>(frame[1]) << 16) | (static_cast<std::uint32_t>(frame[2]) << 8) | frame[3];
}

int typeCode(const Frame& frame) {
    return frame[4] >> 3;
}

std::optional<Identification> decodeIdentification(const Frame& frame) {
    const int tc = typeCode(frame);
    if (tc < 1 || tc > 4) return std::nullopt;
    Identification id;
    id.category = static_cast<int>(bits(frame, 37, 3));
    for (int i = 0; i < 8; ++i) id.callsign.push_back(kCharset[static_cast<std::size_t>(bits(frame, 40 + 6 * i, 6))]);
    while (!id.callsign.empty() && (id.callsign.back() == ' ' || id.callsign.back() == '#')) id.callsign.pop_back();
    return id;
}

std::optional<AirbornePosition> decodePosition(const Frame& frame) {
    const int tc = typeCode(frame);
    if ((tc < 9 || tc > 18) && (tc < 20 || tc > 22)) return std::nullopt;
    AirbornePosition p;
    p.typeCode = tc;
    const auto alt = static_cast<std::uint32_t>(bits(frame, 40, 12));
    if (tc <= 18 && (alt & 0x10U) != 0) {
        const std::uint32_t n = ((alt & 0xFE0U) >> 1) | (alt & 0x0FU);
        p.altitudeFeet = static_cast<int>(n) * 25 - 1000;
    }
    p.odd = bits(frame, 53, 1) == 1;
    p.cprLatitude = static_cast<std::uint32_t>(bits(frame, 54, 17));
    p.cprLongitude = static_cast<std::uint32_t>(bits(frame, 71, 17));
    return p;
}

std::optional<Velocity> decodeVelocity(const Frame& frame) {
    if (typeCode(frame) != 19) return std::nullopt;
    Velocity v;
    v.subtype = static_cast<int>(bits(frame, 37, 3));
    if (v.subtype < 1 || v.subtype > 4) return std::nullopt;
    const bool vrDown = bits(frame, 68, 1) == 1;
    const auto vr = static_cast<int>(bits(frame, 69, 9));
    if (vr != 0) v.verticalRateFpm = (vr - 1) * 64 * (vrDown ? -1 : 1);
    if (v.subtype == 1 || v.subtype == 2) {
        const double scale = v.subtype == 2 ? 4.0 : 1.0;
        const bool west = bits(frame, 45, 1) == 1;
        const auto vew = static_cast<int>(bits(frame, 46, 10));
        const bool south = bits(frame, 56, 1) == 1;
        const auto vns = static_cast<int>(bits(frame, 57, 10));
        if (vew == 0 || vns == 0) return v;
        const double east = (vew - 1) * scale * (west ? -1.0 : 1.0);
        const double north = (vns - 1) * scale * (south ? -1.0 : 1.0);
        v.groundSpeedKnots = std::hypot(east, north);
        v.trackDegrees = mod(std::atan2(east, north) * 180.0 / kPi, 360.0);
    } else {
        const bool headingValid = bits(frame, 45, 1) == 1;
        if (headingValid) v.headingDegrees = static_cast<double>(bits(frame, 46, 10)) * 360.0 / 1024.0;
        const auto as = static_cast<int>(bits(frame, 57, 10));
        if (as != 0) v.airspeedKnots = (as - 1) * (v.subtype == 4 ? 4.0 : 1.0);
    }
    return v;
}

int cprNl(double lat) {
    lat = std::fabs(lat);
    if (lat == 0.0) return kAdsbCprLatitudeOddZones;
    if (lat == kAdsbCprLatitudeTransitionDegrees) return 2;
    if (lat > kAdsbCprLatitudeTransitionDegrees) return 1;
    constexpr double nz = 15.0;
    const double a = 1.0 - std::cos(kPi / (2.0 * nz));
    const double b = std::cos(kPi / 180.0 * lat);
    return static_cast<int>(std::floor(2.0 * kPi / std::acos(1.0 - a / (b * b))));
}

std::optional<std::pair<double, double>> cprGlobal(std::uint32_t evenLat, std::uint32_t evenLon, std::uint32_t oddLat, std::uint32_t oddLon,
                                                   bool oddIsNewest) {
    const double lat0 = evenLat / kAdsbCprScale;
    const double lat1 = oddLat / kAdsbCprScale;
    const double lon0 = evenLon / kAdsbCprScale;
    const double lon1 = oddLon / kAdsbCprScale;
    const double dlat0 = 360.0 / kAdsbCprLatitudeEvenZones;
    const double dlat1 = 360.0 / kAdsbCprLatitudeOddZones;
    const double j = std::floor((kAdsbCprLatitudeOddZones * lat0) - (kAdsbCprLatitudeEvenZones * lat1) + 0.5);
    double latE = dlat0 * (mod(j, kAdsbCprLatitudeEvenZones) + lat0);
    double latO = dlat1 * (mod(j, kAdsbCprLatitudeOddZones) + lat1);
    if (latE >= 270.0) latE -= 360.0;
    if (latO >= 270.0) latO -= 360.0;
    if (latE < -90.0 || latE > 90.0 || latO < -90.0 || latO > 90.0) return std::nullopt;
    if (cprNl(latE) != cprNl(latO)) return std::nullopt;
    double lat;
    double lon;
    if (!oddIsNewest) {
        lat = latE;
        const int ni = std::max(cprNl(latE), 1);
        const double m = std::floor(lon0 * (cprNl(latE) - 1) - lon1 * cprNl(latE) + 0.5);
        lon = (360.0 / ni) * (mod(m, ni) + lon0);
    } else {
        lat = latO;
        const int ni = std::max(cprNl(latO) - 1, 1);
        const double m = std::floor(lon0 * (cprNl(latO) - 1) - lon1 * cprNl(latO) + 0.5);
        lon = (360.0 / ni) * (mod(m, ni) + lon1);
    }
    if (lon >= 180.0) lon -= 360.0;
    return std::pair<double, double>{lat, lon};
}

std::pair<double, double> cprLocal(double refLat, double refLon, std::uint32_t cprLat, std::uint32_t cprLon, bool odd) {
    const double dlat = odd ? 360.0 / kAdsbCprLatitudeOddZones : 360.0 / kAdsbCprLatitudeEvenZones;
    const double latFrac = cprLat / kAdsbCprScale;
    const double lonFrac = cprLon / kAdsbCprScale;
    const double j = std::floor(refLat / dlat) + std::floor(0.5 + mod(refLat, dlat) / dlat - latFrac);
    const double lat = dlat * (j + latFrac);
    const int ni = std::max(cprNl(lat) - (odd ? 1 : 0), 1);
    const double dlon = 360.0 / ni;
    const double m = std::floor(refLon / dlon) + std::floor(0.5 + mod(refLon, dlon) / dlon - lonFrac);
    return {lat, dlon * (m + lonFrac)};
}

json::Value Aircraft::toJson() const {
    json::Value v = json::Value::object();
    v["icao"] = std::format("{:06X}", icao);
    v["callsign"] = callsign;
    if (altitudeFeet) v["altitude_ft"] = *altitudeFeet;
    if (latitude) v["latitude"] = *latitude;
    if (longitude) v["longitude"] = *longitude;
    if (groundSpeedKnots) v["ground_speed_kt"] = *groundSpeedKnots;
    if (trackDegrees) v["track_deg"] = *trackDegrees;
    if (verticalRateFpm) v["vertical_rate_fpm"] = *verticalRateFpm;
    if (squawk) v["squawk"] = *squawk;
    v["messages"] = messages;
    return v;
}

void Tracker::setReference(double latitude, double longitude) {
    reference_ = {latitude, longitude};
}

bool Tracker::ingest(const Frame& frame, std::chrono::steady_clock::time_point now) {
    const int df = downlinkFormat(frame);
    if ((df != 17 && df != 18) || !crcValid(frame)) {
        ++rejected_;
        return false;
    }
    auto& ac = aircraft_[icao(frame)];
    ac.icao = icao(frame);
    ac.lastSeen = now;
    ++ac.messages;
    if (auto id = decodeIdentification(frame)) {
        ac.callsign = id->callsign;
    } else if (auto pos = decodePosition(frame)) {
        if (pos->altitudeFeet) ac.altitudeFeet = pos->altitudeFeet;
        auto& p = pending_[ac.icao];
        if (pos->odd) {
            p.odd = pos;
            p.oddTime = now;
        } else {
            p.even = pos;
            p.evenTime = now;
        }
        if (p.even && p.odd && std::chrono::abs(p.evenTime - p.oddTime) < std::chrono::seconds(10)) {
            if (auto ll =
                    cprGlobal(p.even->cprLatitude, p.even->cprLongitude, p.odd->cprLatitude, p.odd->cprLongitude, p.oddTime > p.evenTime)) {
                ac.latitude = ll->first;
                ac.longitude = ll->second;
            }
        } else if (ac.latitude && ac.longitude) {
            const auto ll = cprLocal(*ac.latitude, *ac.longitude, pos->cprLatitude, pos->cprLongitude, pos->odd);
            ac.latitude = ll.first;
            ac.longitude = ll.second;
        } else if (reference_) {
            const auto ll = cprLocal(reference_->first, reference_->second, pos->cprLatitude, pos->cprLongitude, pos->odd);
            ac.latitude = ll.first;
            ac.longitude = ll.second;
        }
    } else if (auto vel = decodeVelocity(frame)) {
        if (vel->groundSpeedKnots) ac.groundSpeedKnots = vel->groundSpeedKnots;
        if (vel->trackDegrees) ac.trackDegrees = vel->trackDegrees;
        if (!vel->groundSpeedKnots && vel->airspeedKnots) ac.groundSpeedKnots = vel->airspeedKnots;
        if (!vel->trackDegrees && vel->headingDegrees) ac.trackDegrees = vel->headingDegrees;
        if (vel->verticalRateFpm) ac.verticalRateFpm = vel->verticalRateFpm;
    }
    return true;
}

bool Tracker::ingestHex(std::string_view hex, std::chrono::steady_clock::time_point now) {
    auto frame = parseHex(hex);
    if (!frame) {
        ++rejected_;
        return false;
    }
    return ingest(*frame, now);
}

bool Tracker::ingestSbs(std::string_view line, std::chrono::steady_clock::time_point now) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    std::vector<std::string> f;
    std::size_t start = 0;
    while (true) {
        const auto comma = line.find(',', start);
        f.emplace_back(line.substr(start, comma == std::string_view::npos ? comma : comma - start));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    if (f.size() < 11 || f[0] != "MSG" || f[4].size() != 6) return false;
    std::uint32_t id = 0;
    if (std::from_chars(f[4].data(), f[4].data() + 6, id, 16).ec != std::errc()) return false;
    auto num = [&](std::size_t i) -> std::optional<double> {
        if (i >= f.size() || f[i].empty()) return std::nullopt;
        double v = 0;
        if (std::from_chars(f[i].data(), f[i].data() + f[i].size(), v).ec != std::errc()) return std::nullopt;
        return v;
    };
    auto& ac = aircraft_[id];
    ac.icao = id;
    ac.lastSeen = now;
    ++ac.messages;
    if (!f[10].empty()) {
        ac.callsign = f[10];
        while (!ac.callsign.empty() && ac.callsign.back() == ' ') ac.callsign.pop_back();
    }
    if (auto v = num(11)) ac.altitudeFeet = static_cast<int>(*v);
    if (auto v = num(12)) ac.groundSpeedKnots = v;
    if (auto v = num(13)) ac.trackDegrees = v;
    if (auto v = num(14)) ac.latitude = v;
    if (auto v = num(15)) ac.longitude = v;
    if (auto v = num(16)) ac.verticalRateFpm = static_cast<int>(*v);
    if (f.size() > 17 && !f[17].empty()) ac.squawk = f[17];
    return true;
}

std::size_t Tracker::ingestAvrStream(std::string_view data) {
    avrBuffer_.append(data);
    std::size_t accepted = 0;
    std::size_t start = 0;
    while (true) {
        const auto end = avrBuffer_.find(';', start);
        if (end == std::string::npos) break;
        const auto begin = avrBuffer_.rfind('*', end);
        if (begin != std::string::npos && begin >= start) {
            if (ingestHex(std::string_view(avrBuffer_).substr(begin, end - begin + 1))) ++accepted;
        }
        start = end + 1;
    }
    avrBuffer_.erase(0, start);
    if (avrBuffer_.size() > kMaximumAvrBufferBytes) avrBuffer_.clear();
    return accepted;
}

void Tracker::expire(std::chrono::seconds maxAge, std::chrono::steady_clock::time_point now) {
    std::erase_if(aircraft_, [&](const auto& kv) { return now - kv.second.lastSeen > maxAge; });
    std::erase_if(pending_, [&](const auto& kv) { return !aircraft_.contains(kv.first); });
}

std::vector<Aircraft> Tracker::aircraft() const {
    std::vector<Aircraft> out;
    out.reserve(aircraft_.size());
    for (const auto& [id, ac] : aircraft_) out.push_back(ac);
    return out;
}

std::optional<Aircraft> Tracker::find(std::uint32_t icaoAddress) const {
    auto it = aircraft_.find(icaoAddress);
    if (it == aircraft_.end()) return std::nullopt;
    return it->second;
}

json::Value Tracker::toJson() const {
    json::Value list = json::Value::array();
    for (const auto& [id, ac] : aircraft_) list.push(ac.toJson());
    json::Value out = json::Value::object();
    out["aircraft"] = std::move(list);
    out["rejected_frames"] = rejected_;
    return out;
}

} // namespace gygax::robotics::adsb
