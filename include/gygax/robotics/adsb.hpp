#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gygax/core/json.hpp>

namespace gygax::robotics::adsb {

using Frame = std::array<std::uint8_t, 14>;

std::optional<Frame> parseHex(std::string_view hex);
std::uint32_t crc24(const Frame& frame);
bool crcValid(const Frame& frame);
int downlinkFormat(const Frame& frame);
std::uint32_t icao(const Frame& frame);
int typeCode(const Frame& frame);

struct Identification {
    std::string callsign;
    int category = 0;
};

struct AirbornePosition {
    std::optional<int> altitudeFeet;
    bool odd = false;
    std::uint32_t cprLatitude = 0;
    std::uint32_t cprLongitude = 0;
    int typeCode = 0;
};

struct Velocity {
    int subtype = 0;
    std::optional<double> groundSpeedKnots;
    std::optional<double> trackDegrees;
    std::optional<double> airspeedKnots;
    std::optional<double> headingDegrees;
    std::optional<int> verticalRateFpm;
};

std::optional<Identification> decodeIdentification(const Frame& frame);
std::optional<AirbornePosition> decodePosition(const Frame& frame);
std::optional<Velocity> decodeVelocity(const Frame& frame);

int cprNl(double latitude);
std::optional<std::pair<double, double>> cprGlobal(std::uint32_t evenLat, std::uint32_t evenLon, std::uint32_t oddLat, std::uint32_t oddLon,
                                                   bool oddIsNewest);
std::pair<double, double> cprLocal(double referenceLat, double referenceLon, std::uint32_t cprLat, std::uint32_t cprLon, bool odd);

struct Aircraft {
    std::uint32_t icao = 0;
    std::string callsign;
    std::optional<int> altitudeFeet;
    std::optional<double> latitude;
    std::optional<double> longitude;
    std::optional<double> groundSpeedKnots;
    std::optional<double> trackDegrees;
    std::optional<int> verticalRateFpm;
    std::optional<std::string> squawk;
    std::uint64_t messages = 0;
    std::chrono::steady_clock::time_point lastSeen{};
    json::Value toJson() const;
};

class Tracker {
public:
    bool ingest(const Frame& frame, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    bool ingestHex(std::string_view hex, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    bool ingestSbs(std::string_view line, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    std::size_t ingestAvrStream(std::string_view data);
    void expire(std::chrono::seconds maxAge, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void setReference(double latitude, double longitude);

    [[nodiscard]] std::vector<Aircraft> aircraft() const;
    [[nodiscard]] std::optional<Aircraft> find(std::uint32_t icao) const;
    [[nodiscard]] json::Value toJson() const;
    [[nodiscard]] std::uint64_t rejectedFrames() const { return rejected_; }

private:
    struct Pending {
        std::optional<AirbornePosition> even;
        std::optional<AirbornePosition> odd;
        std::chrono::steady_clock::time_point evenTime{};
        std::chrono::steady_clock::time_point oddTime{};
    };

    std::map<std::uint32_t, Aircraft> aircraft_;
    std::map<std::uint32_t, Pending> pending_;
    std::optional<std::pair<double, double>> reference_;
    std::uint64_t rejected_ = 0;
    std::string avrBuffer_;
};

} // namespace gygax::robotics::adsb
