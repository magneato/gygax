#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gygax/core/json.hpp>
#include <gygax/net/link.hpp>

namespace gygax::robotics::mavlink {

using net::IOResult;

inline constexpr std::uint8_t kMavlinkV2Version = 2;
inline constexpr std::uint8_t kDefaultGroundStationSystemId = 255;
inline constexpr std::uint8_t kDefaultGroundStationComponentId = 190;
inline constexpr std::uint8_t kDefaultTargetSystemId = 1;
inline constexpr std::uint8_t kDefaultTargetComponentId = 1;
inline constexpr std::uint16_t kMavlinkCrcInitialValue = 0xFFFF;

enum class FieldType { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, CHAR };

struct FieldDef {
    const char* name;
    FieldType type;
    int arrayLength;
    int offset;
    bool extension;
};

struct MessageDef {
    std::uint32_t id;
    const char* name;
    std::uint8_t crcExtra;
    std::uint16_t baseLength;
    std::uint16_t maxLength;
    std::vector<FieldDef> fields;
};

const MessageDef* findMessage(std::uint32_t id);
const MessageDef* findMessage(std::string_view name);
std::size_t messageCount();

struct Frame {
    std::uint8_t version = kMavlinkV2Version;
    std::uint8_t sequence = 0;
    std::uint8_t systemId = kDefaultTargetSystemId;
    std::uint8_t componentId = kDefaultTargetComponentId;
    std::uint32_t messageId = 0;
    std::vector<std::uint8_t> payload;
    bool signedFrame = false;
};

json::Value decodePayload(const MessageDef& def, std::span<const std::uint8_t> payload);
std::optional<std::vector<std::uint8_t>> encodePayload(const MessageDef& def, const json::Value& fields, std::string* error = nullptr);
std::vector<std::uint8_t> serialize(const Frame& frame, bool truncateZeros = true);
std::uint16_t crc16(std::span<const std::uint8_t> data, std::uint16_t seed = kMavlinkCrcInitialValue);

struct ParserStats {
    std::uint64_t bytes = 0;
    std::uint64_t frames = 0;
    std::uint64_t crcErrors = 0;
    std::uint64_t unknownMessages = 0;
    std::uint64_t resyncs = 0;
};

class Parser {
public:
    std::vector<Frame> feed(std::span<const std::uint8_t> data);
    [[nodiscard]] const ParserStats& stats() const { return stats_; }

private:
    std::vector<std::uint8_t> buffer_;
    ParserStats stats_;
};

constexpr std::uint16_t kCmdNavReturnToLaunch = 20;
constexpr std::uint16_t kCmdNavLand = 21;
constexpr std::uint16_t kCmdNavTakeoff = 22;
constexpr std::uint16_t kCmdDoSetMode = 176;
constexpr std::uint16_t kCmdDoChangeSpeed = 178;
constexpr std::uint16_t kCmdDoReposition = 192;
constexpr std::uint16_t kCmdSetMessageInterval = 511;
constexpr std::uint16_t kCmdComponentArmDisarm = 400;

struct VehicleState {
    bool connected = false;
    std::uint8_t systemId = 0;
    std::uint8_t componentId = 0;
    std::uint8_t vehicleType = 0;
    std::uint8_t autopilot = 0;
    std::uint8_t baseMode = 0;
    std::uint32_t customMode = 0;
    std::uint8_t systemStatus = 0;
    bool armed = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitudeMsl = 0.0;
    double altitudeRelative = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double vz = 0.0;
    double headingDeg = 0.0;
    double roll = 0.0;
    double pitch = 0.0;
    double yaw = 0.0;
    double groundspeed = 0.0;
    double airspeed = 0.0;
    double climb = 0.0;
    double batteryVoltage = 0.0;
    double batteryCurrent = 0.0;
    int batteryRemaining = -1;
    int gpsFix = 0;
    int satellites = 0;
    std::string lastStatusText;
    std::uint64_t heartbeats = 0;
    std::uint64_t messages = 0;
    json::Value toJson() const;
};

struct VehicleOptions {
    std::uint8_t ourSystemId = 255;
    std::uint8_t ourComponentId = 190;
    std::uint8_t targetSystem = 1;
    std::uint8_t targetComponent = 1;
    std::chrono::milliseconds heartbeatInterval{1000};
    std::chrono::milliseconds heartbeatTimeout{5000};
    std::chrono::milliseconds commandTimeout{3000};
    bool followFirstVehicle = true;
};

class Vehicle {
public:
    Vehicle(std::shared_ptr<net::ByteLink> link, VehicleOptions options = {});
    ~Vehicle();
    Vehicle(const Vehicle&) = delete;
    Vehicle& operator=(const Vehicle&) = delete;

    void start();
    void stop();

    [[nodiscard]] VehicleState state() const;
    [[nodiscard]] bool waitForHeartbeat(std::chrono::milliseconds timeout);

    [[nodiscard]] IOResult sendMessage(std::string_view name, const json::Value& fields);
    [[nodiscard]] IOResult sendCommand(std::uint16_t command, const std::array<double, 7>& params, std::uint8_t* result = nullptr);
    [[nodiscard]] IOResult arm(bool force = false);
    [[nodiscard]] IOResult disarm(bool force = false);
    [[nodiscard]] IOResult setMode(std::uint32_t customMode);
    [[nodiscard]] IOResult takeoff(double altitudeMeters);
    [[nodiscard]] IOResult land();
    [[nodiscard]] IOResult returnToLaunch();
    [[nodiscard]] IOResult gotoGlobal(double latitude, double longitude, double relativeAltitude);
    [[nodiscard]] IOResult requestMessageRate(std::uint32_t messageId, double hertz);
    [[nodiscard]] IOResult setParameter(const std::string& name, double value, std::uint8_t type = 9);
    [[nodiscard]] IOResult readParameter(const std::string& name, double& value,
                                         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    void onMessage(std::function<void(const std::string& name, const Frame& frame, const json::Value& fields)> handler);
    [[nodiscard]] std::optional<json::Value> lastMessage(const std::string& name) const;
    [[nodiscard]] ParserStats parserStats() const;

    static std::optional<std::uint32_t> arducopterMode(std::string_view name);

private:
    void loop(const std::stop_token& stop);
    void handle(const Frame& frame);
    IOResult writeFrame(std::uint32_t messageId, const std::vector<std::uint8_t>& payload);

    std::shared_ptr<net::ByteLink> link_;
    VehicleOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    VehicleState state_;
    std::map<std::string, json::Value> last_;
    std::function<void(const std::string&, const Frame&, const json::Value&)> handler_;
    std::optional<std::pair<std::uint16_t, std::uint8_t>> ack_;
    std::optional<std::pair<std::string, double>> param_;
    std::atomic<std::uint8_t> sequence_{0};
    std::chrono::steady_clock::time_point lastHeartbeat_{};
    Parser parser_;
    std::mutex writeMutex_;
    std::jthread thread_;
};

} // namespace gygax::robotics::mavlink
