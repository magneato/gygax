#include <gygax/robotics/nmea.hpp>

#include <charconv>

#include <gygax/core/charconv.hpp>
#include <cmath>
#include <cstdio>
#include <format>

namespace gygax::robotics::nmea {

namespace {

constexpr std::size_t kMinimumSentenceLength = 6;
constexpr std::size_t kMinimumTalkerAndSentenceTypeLength = 3;
constexpr std::size_t kMaximumTalkerAndSentenceTypeLength = 6;
constexpr std::size_t kChecksumHexCharacterCount = 2;
constexpr std::size_t kMaximumBufferedSentenceBytes = 1024;
constexpr double kMinutesPerDegree = 60.0;
constexpr double kKilometersPerHourPerKnot = 1.852;
constexpr double kKnotsPerMeterPerSecond = 1.943844;

std::optional<double> number(const std::vector<std::string>& f, std::size_t i) {
    if (i >= f.size() || f[i].empty()) return std::nullopt;
    double v = 0.0;
    auto [ptr, ec] = gygax::fromChars(f[i].data(), f[i].data() + f[i].size(), v);
    if (ec != std::errc() || ptr != f[i].data() + f[i].size()) return std::nullopt;
    return v;
}

int integer(const std::vector<std::string>& f, std::size_t i) {
    if (i >= f.size() || f[i].empty()) return 0;
    int v = 0;
    gygax::fromChars(f[i].data(), f[i].data() + f[i].size(), v);
    return v;
}

std::string field(const std::vector<std::string>& f, std::size_t i) {
    return i < f.size() ? f[i] : std::string();
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

} // namespace

std::uint8_t checksum(std::string_view body) {
    std::uint8_t cs = 0;
    for (const char c : body) cs ^= static_cast<std::uint8_t>(c);
    return cs;
}

std::optional<Sentence> parse(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.remove_suffix(1);
    if (line.size() < kMinimumSentenceLength || (line.front() != '$' && line.front() != '!')) return std::nullopt;
    Sentence s;
    std::string_view body = line.substr(1);
    if (const auto star = body.rfind('*'); star != std::string_view::npos) {
        if (star + 1 + kChecksumHexCharacterCount != body.size()) return std::nullopt;
        const int hi = hexNibble(body[star + 1]);
        const int lo = hexNibble(body[star + 2]);
        if (hi < 0 || lo < 0) return std::nullopt;
        s.checksumPresent = true;
        s.checksumValid = checksum(body.substr(0, star)) == static_cast<std::uint8_t>(hi * 16 + lo);
        body = body.substr(0, star);
    }
    const auto comma = body.find(',');
    const std::string_view head = body.substr(0, comma);
    if (head.size() < kMinimumTalkerAndSentenceTypeLength || head.size() > kMaximumTalkerAndSentenceTypeLength) return std::nullopt;
    if (head.front() == 'P') {
        s.talker = "P";
        s.type = std::string(head.substr(1));
    } else {
        s.talker = std::string(head.substr(0, 2));
        s.type = std::string(head.substr(2));
    }
    if (comma != std::string_view::npos) {
        std::string_view rest = body.substr(comma + 1);
        while (true) {
            const auto next = rest.find(',');
            s.fields.emplace_back(rest.substr(0, next));
            if (next == std::string_view::npos) break;
            rest.remove_prefix(next + 1);
        }
    }
    return s;
}

std::string build(std::string_view talker, std::string_view type, const std::vector<std::string>& fields) {
    std::string body = std::string(talker) + std::string(type);
    for (const auto& f : fields) {
        body += ',';
        body += f;
    }
    return std::format("${}*{:02X}", body, checksum(body));
}

std::optional<double> parseCoordinate(std::string_view value, std::string_view hemisphere) {
    if (value.empty() || hemisphere.size() != 1) return std::nullopt;
    const auto dot = value.find('.');
    if (dot == std::string_view::npos || dot < 3) return std::nullopt;
    double minutes = 0.0;
    int degrees = 0;
    const auto degText = value.substr(0, dot - 2);
    const auto minText = value.substr(dot - 2);
    if (gygax::fromChars(degText.data(), degText.data() + degText.size(), degrees).ec != std::errc()) return std::nullopt;
    if (gygax::fromChars(minText.data(), minText.data() + minText.size(), minutes).ec != std::errc()) return std::nullopt;
    double result = degrees + minutes / kMinutesPerDegree;
    const char h = hemisphere[0];
    if (h == 'S' || h == 'W')
        result = -result;
    else if (h != 'N' && h != 'E')
        return std::nullopt;
    return result;
}

std::string formatCoordinate(double degrees, bool latitude) {
    const double absolute = std::fabs(degrees);
    const int whole = static_cast<int>(absolute);
    const double minutes = (absolute - whole) * kMinutesPerDegree;
    const char hemisphere = latitude ? (degrees < 0 ? 'S' : 'N') : (degrees < 0 ? 'W' : 'E');
    return latitude ? std::format("{:02d}{:07.4f},{}", whole, minutes, hemisphere)
                    : std::format("{:03d}{:07.4f},{}", whole, minutes, hemisphere);
}

bool State::update(const Sentence& s) {
    if (s.checksumPresent && !s.checksumValid) return false;
    const auto& f = s.fields;
    if (s.type == "GGA" && f.size() >= 9) {
        fix_.utcTime = field(f, 0);
        fix_.quality = integer(f, 5);
        fix_.satellites = integer(f, 6);
        fix_.hdop = number(f, 7).value_or(0.0);
        fix_.altitude = number(f, 8).value_or(0.0);
        if (auto lat = parseCoordinate(field(f, 1), field(f, 2)); lat && fix_.quality > 0) fix_.latitude = *lat;
        if (auto lon = parseCoordinate(field(f, 3), field(f, 4)); lon && fix_.quality > 0) fix_.longitude = *lon;
        fix_.valid = fix_.quality > 0;
        return true;
    }
    if (s.type == "RMC" && f.size() >= 9) {
        fix_.utcTime = field(f, 0);
        fix_.utcDate = field(f, 8);
        const bool active = field(f, 1) == "A";
        if (active) {
            if (auto lat = parseCoordinate(field(f, 2), field(f, 3))) fix_.latitude = *lat;
            if (auto lon = parseCoordinate(field(f, 4), field(f, 5))) fix_.longitude = *lon;
            fix_.speedKnots = number(f, 6).value_or(0.0);
            fix_.courseDegrees = number(f, 7).value_or(0.0);
        }
        if (auto mv = number(f, 9)) fix_.magneticVariation = field(f, 10) == "W" ? -*mv : *mv;
        fix_.valid = active;
        return true;
    }
    if (s.type == "VTG" && f.size() >= 7) {
        if (auto course = number(f, 0)) fix_.courseDegrees = *course;
        if (auto speed = number(f, 4)) fix_.speedKnots = *speed;
        return true;
    }
    if (s.type == "GLL" && f.size() >= 6) {
        if (field(f, 5) == "A") {
            if (auto lat = parseCoordinate(field(f, 0), field(f, 1))) fix_.latitude = *lat;
            if (auto lon = parseCoordinate(field(f, 2), field(f, 3))) fix_.longitude = *lon;
            fix_.valid = true;
        }
        return true;
    }
    if (s.type == "HDT" && !f.empty()) {
        headingTrue = number(f, 0);
        return true;
    }
    if (s.type == "HDM" && !f.empty()) {
        headingMagnetic = number(f, 0);
        return true;
    }
    if (s.type == "HDG" && !f.empty()) {
        headingMagnetic = number(f, 0);
        return true;
    }
    if (s.type == "DBT" && f.size() >= 3) {
        depthMeters = number(f, 2);
        return true;
    }
    if (s.type == "MTW" && !f.empty()) {
        waterTemperature = number(f, 0);
        return true;
    }
    if (s.type == "MWV" && f.size() >= 5 && field(f, 4) == "A") {
        windAngle = number(f, 0);
        auto speed = number(f, 2);
        const auto unit = field(f, 3);
        if (speed) windSpeed = unit == "K" ? *speed / kKilometersPerHourPerKnot : (unit == "M" ? *speed * kKnotsPerMeterPerSecond : *speed);
        return true;
    }
    if (s.type == "VHW" && f.size() >= 6) {
        speedThroughWaterKnots = number(f, 4);
        return true;
    }
    return false;
}

json::Value State::toJson() const {
    json::Value v = json::Value::object();
    v["valid"] = fix_.valid;
    v["latitude"] = fix_.latitude;
    v["longitude"] = fix_.longitude;
    v["altitude_m"] = fix_.altitude;
    v["quality"] = fix_.quality;
    v["satellites"] = fix_.satellites;
    v["hdop"] = fix_.hdop;
    v["speed_knots"] = fix_.speedKnots;
    v["course_deg"] = fix_.courseDegrees;
    v["utc_time"] = fix_.utcTime;
    v["utc_date"] = fix_.utcDate;
    auto put = [&](const char* key, const std::optional<double>& value) {
        if (value) v[key] = *value;
    };
    put("heading_true_deg", headingTrue);
    put("heading_magnetic_deg", headingMagnetic);
    put("depth_m", depthMeters);
    put("water_temperature_c", waterTemperature);
    put("wind_angle_deg", windAngle);
    put("wind_speed_knots", windSpeed);
    put("speed_through_water_knots", speedThroughWaterKnots);
    return v;
}

std::vector<std::string> LineAssembler::feed(std::string_view data) {
    std::vector<std::string> lines;
    buffer_.append(data);
    std::size_t start = 0;
    while (true) {
        const auto nl = buffer_.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = buffer_.substr(start, nl - start);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
        start = nl + 1;
    }
    buffer_.erase(0, start);
    if (buffer_.size() > kMaximumBufferedSentenceBytes) buffer_.clear();
    return lines;
}

} // namespace gygax::robotics::nmea
