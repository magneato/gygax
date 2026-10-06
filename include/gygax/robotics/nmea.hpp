#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gygax/core/json.hpp>

namespace gygax::robotics::nmea {

struct Sentence {
    std::string talker;
    std::string type;
    std::vector<std::string> fields;
    bool checksumPresent = false;
    bool checksumValid = false;
};

std::uint8_t checksum(std::string_view body);
std::optional<Sentence> parse(std::string_view line);
std::string build(std::string_view talker, std::string_view type, const std::vector<std::string>& fields);

struct Fix {
    bool valid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    int quality = 0;
    int satellites = 0;
    double hdop = 0.0;
    double speedKnots = 0.0;
    double courseDegrees = 0.0;
    std::string utcTime;
    std::string utcDate;
    double magneticVariation = 0.0;
};

std::optional<double> parseCoordinate(std::string_view value, std::string_view hemisphere);
std::string formatCoordinate(double degrees, bool latitude);

class State {
public:
    bool update(const Sentence& sentence);
    [[nodiscard]] json::Value toJson() const;
    [[nodiscard]] const Fix& fix() const { return fix_; }

    std::optional<double> headingTrue;
    std::optional<double> headingMagnetic;
    std::optional<double> depthMeters;
    std::optional<double> waterTemperature;
    std::optional<double> windAngle;
    std::optional<double> windSpeed;
    std::optional<double> speedThroughWaterKnots;

private:
    Fix fix_;
};

class LineAssembler {
public:
    std::vector<std::string> feed(std::string_view data);

private:
    std::string buffer_;
};

} // namespace gygax::robotics::nmea
