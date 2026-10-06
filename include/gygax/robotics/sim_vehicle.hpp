#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <gygax/core/json.hpp>
#include <gygax/net/link.hpp>
#include <gygax/robotics/mavlink.hpp>

namespace gygax::robotics {

inline constexpr std::uint8_t kDefaultAutopilotSystemId = 1;
inline constexpr std::uint8_t kDefaultAutopilotComponentId = 1;

struct VirtualAutopilotOptions {
    std::uint8_t systemId = kDefaultAutopilotSystemId;
    std::uint8_t componentId = kDefaultAutopilotComponentId;
    double homeLatitude = 47.397742;
    double homeLongitude = 8.545594;
    double homeAltitudeMsl = 488.0;
    double climbRate = 2.5;
    double cruiseSpeed = 5.0;
    double descentRate = 1.0;
    std::uint8_t vehicleType = 2;
};

struct VirtualAutopilotState {
    bool armed = false;
    std::uint32_t mode = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    double relativeAltitude = 0.0;
    double groundSpeed = 0.0;
    double headingDegrees = 0.0;
    double batteryPercent = 100.0;
    bool flying = false;
};

class VirtualAutopilot {
public:
    VirtualAutopilot(std::shared_ptr<net::ByteLink> link, const VirtualAutopilotOptions& options = {});
    ~VirtualAutopilot();
    VirtualAutopilot(const VirtualAutopilot&) = delete;
    VirtualAutopilot& operator=(const VirtualAutopilot&) = delete;

    void start();
    void stop();
    void step(double dtSeconds);
    void poll(std::chrono::milliseconds timeout);
    [[nodiscard]] VirtualAutopilotState state() const;
    [[nodiscard]] std::uint64_t commandsHandled() const { return commands_.load(); }

    static constexpr std::uint32_t kModeStabilize = 0;
    static constexpr std::uint32_t kModeGuided = 4;
    static constexpr std::uint32_t kModeLoiter = 5;
    static constexpr std::uint32_t kModeRtl = 6;
    static constexpr std::uint32_t kModeLand = 9;

private:
    struct Target {
        bool active = false;
        double latitude = 0.0;
        double longitude = 0.0;
        double altitude = 0.0;
    };

    void loop(const std::stop_token& stop);
    void handle(const mavlink::Frame& frame);
    void handleCommand(const json::Value& f);
    void send(const char* name, const json::Value& fields);
    void emitTelemetry();
    void statusText(const std::string& text, int severity = 6);
    void ack(std::uint16_t command, std::uint8_t result);
    void sendParam(const std::string& id, double value, int index);

    std::shared_ptr<net::ByteLink> link_;
    VirtualAutopilotOptions options_;
    mutable std::mutex mutex_;
    VirtualAutopilotState state_;
    Target target_;
    double takeoffAltitude_ = 0.0;
    double roll_ = 0.0;
    double pitch_ = 0.0;
    double vNorth_ = 0.0;
    double vEast_ = 0.0;
    double climb_ = 0.0;
    double timeSeconds_ = 0.0;
    std::map<std::string, double> params_;
    std::map<std::uint32_t, double> intervals_;
    std::map<std::uint32_t, double> lastSent_;
    std::uint8_t sequence_ = 0;
    std::atomic<std::uint64_t> commands_{0};
    mavlink::Parser parser_;
    std::jthread thread_;
};

} // namespace gygax::robotics
