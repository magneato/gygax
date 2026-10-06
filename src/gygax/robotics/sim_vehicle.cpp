#include <gygax/robotics/sim_vehicle.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gygax::robotics {

namespace {

constexpr double kMetersPerDegree = 111319.5;
constexpr std::uint32_t kHeartbeatMessageId = 0;
constexpr std::uint32_t kSystemStatusMessageId = 1;
constexpr std::uint32_t kGpsRawIntMessageId = 24;
constexpr std::uint32_t kAttitudeMessageId = 30;
constexpr std::uint32_t kGlobalPositionIntMessageId = 33;
constexpr std::uint32_t kVfrHudMessageId = 74;
constexpr std::chrono::milliseconds kAutopilotPollInterval{20};
constexpr std::size_t kMaximumStatusTextBytes = 50;
constexpr double kDefaultHeartbeatIntervalSeconds = 1.0;
constexpr double kGlobalPositionIntervalSeconds = 0.1;
constexpr double kAttitudeIntervalSeconds = 0.1;
constexpr double kVfrHudIntervalSeconds = 0.2;
constexpr double kStatusTelemetryIntervalSeconds = 0.5;
constexpr double kArmCommandThreshold = 0.5;
constexpr double kLowBatteryFailsafePercent = 10.0;
constexpr double kForceDisarmParameter = 21196.0;
constexpr double kMinimumHorizontalTargetDistanceMeters = 0.5;
constexpr double kRtlLandingThresholdMeters = 15.0;
constexpr double kHeadingWrapDegrees = 360.0;
constexpr double kMavlinkPositionScale = 1.0e7;
constexpr double kMavlinkAltitudeScale = 1000.0;
constexpr double kMavlinkVelocityScale = 100.0;
constexpr double kMavlinkHeadingScale = 100.0;
constexpr double kMavlinkBootTimeScale = 1000.0;
constexpr double kMavlinkMicrosecondsPerSecond = 1.0e6;

double toRadians(double d) {
    return d * std::numbers::pi / 180.0;
}
double toDegrees(double r) {
    return r * 180.0 / std::numbers::pi;
}

} // namespace

VirtualAutopilot::VirtualAutopilot(std::shared_ptr<net::ByteLink> link, const VirtualAutopilotOptions& options)
    : link_(std::move(link)), options_(options),
      params_{{"BATT_CAPACITY", 3300.0}, {"WPNAV_SPEED", 500.0}, {"ARMING_CHECK", 1.0}, {"FS_BATT_ENABLE", 0.0}},
      intervals_{{kHeartbeatMessageId, kDefaultHeartbeatIntervalSeconds},
                 {kGlobalPositionIntMessageId, kGlobalPositionIntervalSeconds},
                 {kAttitudeMessageId, kAttitudeIntervalSeconds},
                 {kVfrHudMessageId, kVfrHudIntervalSeconds},
                 {kSystemStatusMessageId, kStatusTelemetryIntervalSeconds},
                 {kGpsRawIntMessageId, kStatusTelemetryIntervalSeconds}} {
    state_.latitude = options_.homeLatitude;
    state_.longitude = options_.homeLongitude;
}

VirtualAutopilot::~VirtualAutopilot() {
    stop();
}

void VirtualAutopilot::start() {
    if (thread_.joinable()) return;
    thread_ = std::jthread([this](const std::stop_token& st) { loop(st); });
}

void VirtualAutopilot::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

VirtualAutopilotState VirtualAutopilot::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void VirtualAutopilot::loop(const std::stop_token& stop) {
    auto last = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
        poll(kAutopilotPollInterval);
        const auto now = std::chrono::steady_clock::now();
        step(std::chrono::duration<double>(now - last).count());
        last = now;
    }
}

void VirtualAutopilot::poll(std::chrono::milliseconds timeout) {
    std::vector<std::uint8_t> chunk;
    if (link_->read(chunk, timeout) != 0) return;
    std::vector<mavlink::Frame> frames;
    {
        std::lock_guard lock(mutex_);
        frames = parser_.feed(chunk);
    }
    for (const auto& f : frames) handle(f);
}

void VirtualAutopilot::send(const char* name, const json::Value& fields) {
    const auto* def = mavlink::findMessage(name);
    auto payload = mavlink::encodePayload(*def, fields);
    if (!payload) return;
    mavlink::Frame f;
    f.sequence = sequence_++;
    f.systemId = options_.systemId;
    f.componentId = options_.componentId;
    f.messageId = def->id;
    f.payload = std::move(*payload);
    (void)link_->write(mavlink::serialize(f));
}

void VirtualAutopilot::statusText(const std::string& text, int severity) {
    json::Value f = json::Value::object();
    f["severity"] = severity;
    f["text"] = text.substr(0, kMaximumStatusTextBytes);
    send("STATUSTEXT", f);
}

void VirtualAutopilot::ack(std::uint16_t command, std::uint8_t result) {
    json::Value f = json::Value::object();
    f["command"] = command;
    f["result"] = result;
    send("COMMAND_ACK", f);
}

void VirtualAutopilot::sendParam(const std::string& id, double value, int index) {
    json::Value f = json::Value::object();
    f["param_id"] = id;
    f["param_value"] = value;
    f["param_type"] = 9;
    f["param_count"] = static_cast<std::int64_t>(params_.size());
    f["param_index"] = index;
    send("PARAM_VALUE", f);
}

void VirtualAutopilot::handle(const mavlink::Frame& frame) {
    const auto* def = mavlink::findMessage(frame.messageId);
    if (def == nullptr) return;
    const auto fields = mavlink::decodePayload(*def, frame.payload);
    const std::string name = def->name;
    std::lock_guard lock(mutex_);
    if (name == "COMMAND_LONG") {
        const auto target = fields.getInt("target_system");
        if (target != 0 && target != options_.systemId) return;
        ++commands_;
        handleCommand(fields);
    } else if (name == "SET_POSITION_TARGET_GLOBAL_INT") {
        if (state_.mode != kModeGuided || !state_.armed) return;
        target_ = {true, static_cast<double>(fields.getInt("lat_int")) / 1e7, static_cast<double>(fields.getInt("lon_int")) / 1e7,
                   fields.getDouble("alt")};
    } else if (name == "PARAM_REQUEST_READ") {
        const auto id = fields.getString("param_id");
        if (auto it = params_.find(id); it != params_.end())
            sendParam(id, it->second, static_cast<int>(std::distance(params_.begin(), it)));
    } else if (name == "PARAM_REQUEST_LIST") {
        int i = 0;
        for (const auto& [id, value] : params_) sendParam(id, value, i++);
    } else if (name == "PARAM_SET") {
        const auto id = fields.getString("param_id");
        if (auto it = params_.find(id); it != params_.end()) {
            it->second = fields.getDouble("param_value");
            sendParam(id, it->second, static_cast<int>(std::distance(params_.begin(), it)));
        }
    }
}

void VirtualAutopilot::handleCommand(const json::Value& f) {
    const auto command = static_cast<std::uint16_t>(f.getInt("command"));
    const double p1 = f.getDouble("param1");
    const double p2 = f.getDouble("param2");
    const double p7 = f.getDouble("param7");
    constexpr std::uint8_t accepted = 0;
    constexpr std::uint8_t failed = 4;
    constexpr std::uint8_t unsupported = 3;
    switch (command) {
    case mavlink::kCmdComponentArmDisarm:
        if (p1 >= kArmCommandThreshold) {
            if (state_.armed) {
                ack(command, accepted);
            } else if (params_["ARMING_CHECK"] != 0.0 && state_.batteryPercent < kLowBatteryFailsafePercent) {
                statusText("PreArm: Battery level too low", 4);
                ack(command, failed);
            } else {
                state_.armed = true;
                statusText("Armed");
                ack(command, accepted);
            }
        } else if (state_.flying && p2 != kForceDisarmParameter) {
            statusText("Disarm rejected: vehicle is flying", 4);
            ack(command, failed);
        } else {
            state_.armed = false;
            state_.flying = false;
            statusText("Disarmed");
            ack(command, accepted);
        }
        break;
    case mavlink::kCmdDoSetMode:
        state_.mode = static_cast<std::uint32_t>(std::max(0.0, p2));
        target_.active = false;
        ack(command, accepted);
        break;
    case mavlink::kCmdNavTakeoff:
        if (!state_.armed || state_.mode != kModeGuided || p7 <= 0.0) {
            statusText("Takeoff rejected: arm in GUIDED mode first", 4);
            ack(command, failed);
        } else {
            takeoffAltitude_ = p7;
            target_ = {true, state_.latitude, state_.longitude, p7};
            state_.flying = true;
            statusText("Takeoff");
            ack(command, accepted);
        }
        break;
    case mavlink::kCmdNavLand:
        state_.mode = kModeLand;
        target_.active = false;
        ack(command, accepted);
        break;
    case mavlink::kCmdNavReturnToLaunch:
        state_.mode = kModeRtl;
        target_ = {true, options_.homeLatitude, options_.homeLongitude, std::max(state_.relativeAltitude, kRtlLandingThresholdMeters)};
        ack(command, accepted);
        break;
    case mavlink::kCmdSetMessageInterval: {
        const auto id = static_cast<std::uint32_t>(p1);
        if (p2 < 0.0)
            intervals_.erase(id);
        else if (p2 > 0.0)
            intervals_[id] = p2 / 1e6;
        ack(command, accepted);
        break;
    }
    default: ack(command, unsupported); break;
    }
}

void VirtualAutopilot::step(double dt) {
    if (dt <= 0.0) return;
    {
        std::lock_guard lock(mutex_);
        timeSeconds_ += dt;
        double targetAlt = state_.relativeAltitude;
        bool moveHorizontal = false;
        if (state_.armed && state_.flying) {
            if (state_.mode == kModeLand) {
                targetAlt = 0.0;
            } else if (target_.active) {
                targetAlt = target_.altitude;
                moveHorizontal = state_.mode == kModeGuided || state_.mode == kModeRtl;
            }
        }
        const double dAlt = targetAlt - state_.relativeAltitude;
        const double rate = dAlt > 0.0 ? options_.climbRate : options_.descentRate * (state_.mode == kModeLand ? 1.0 : 2.0);
        const double step = std::clamp(dAlt, -rate * dt, rate * dt);
        state_.relativeAltitude += step;
        climb_ = dt > 0.0 ? step / dt : 0.0;

        vNorth_ = vEast_ = 0.0;
        if (moveHorizontal) {
            const double dNorth = (target_.latitude - state_.latitude) * kMetersPerDegree;
            const double dEast = (target_.longitude - state_.longitude) * kMetersPerDegree * std::cos(toRadians(state_.latitude));
            const double distance = std::hypot(dNorth, dEast);
            if (distance > kMinimumHorizontalTargetDistanceMeters) {
                const double speed = std::min(options_.cruiseSpeed, distance / dt);
                vNorth_ = dNorth / distance * speed;
                vEast_ = dEast / distance * speed;
                state_.latitude += vNorth_ * dt / kMetersPerDegree;
                state_.longitude += vEast_ * dt / (kMetersPerDegree * std::cos(toRadians(state_.latitude)));
                state_.headingDegrees = std::fmod(toDegrees(std::atan2(vEast_, vNorth_)) + kHeadingWrapDegrees, kHeadingWrapDegrees);
            } else if (state_.mode == kModeRtl) {
                state_.mode = kModeLand;
                target_.active = false;
            }
        }
        state_.groundSpeed = std::hypot(vNorth_, vEast_);
        roll_ = std::clamp(vEast_ * 0.02, -0.4, 0.4);
        pitch_ = std::clamp(-vNorth_ * 0.02, -0.4, 0.4);
        if (state_.flying) {
            state_.batteryPercent = std::max(0.0, state_.batteryPercent - 0.02 * dt);
            if (state_.relativeAltitude <= 0.05 && (state_.mode == kModeLand || (target_.active && target_.altitude <= 0.1))) {
                state_.relativeAltitude = 0.0;
                state_.flying = false;
                state_.armed = false;
                statusText("Landed and disarmed");
            }
        }
    }
    std::lock_guard lock(mutex_);
    emitTelemetry();
}

void VirtualAutopilot::emitTelemetry() {
    auto due = [&](std::uint32_t id) {
        auto it = intervals_.find(id);
        if (it == intervals_.end()) return false;
        const double period = it->second;
        double& last = lastSent_[id];
        if (timeSeconds_ - last < period) return false;
        last = timeSeconds_;
        return true;
    };
    if (due(kHeartbeatMessageId)) {
        json::Value f = json::Value::object();
        f["type"] = options_.vehicleType;
        f["autopilot"] = 3;
        f["base_mode"] = 1 | (state_.mode == kModeGuided ? 8 : 16) | (state_.armed ? 128 : 0);
        f["custom_mode"] = state_.mode;
        f["system_status"] = state_.flying ? 4 : 3;
        f["mavlink_version"] = 3;
        send("HEARTBEAT", f);
    }
    if (due(kGlobalPositionIntMessageId)) {
        json::Value f = json::Value::object();
        f["time_boot_ms"] = static_cast<std::int64_t>(timeSeconds_ * kMavlinkBootTimeScale);
        f["lat"] = static_cast<std::int64_t>(std::llround(state_.latitude * kMavlinkPositionScale));
        f["lon"] = static_cast<std::int64_t>(std::llround(state_.longitude * kMavlinkPositionScale));
        f["alt"] = static_cast<std::int64_t>(std::llround((options_.homeAltitudeMsl + state_.relativeAltitude) * kMavlinkAltitudeScale));
        f["relative_alt"] = static_cast<std::int64_t>(std::llround(state_.relativeAltitude * kMavlinkAltitudeScale));
        f["vx"] = static_cast<std::int64_t>(std::llround(vNorth_ * kMavlinkVelocityScale));
        f["vy"] = static_cast<std::int64_t>(std::llround(vEast_ * kMavlinkVelocityScale));
        f["vz"] = static_cast<std::int64_t>(std::llround(-climb_ * kMavlinkVelocityScale));
        f["hdg"] = static_cast<std::int64_t>(std::llround(state_.headingDegrees * kMavlinkHeadingScale));
        send("GLOBAL_POSITION_INT", f);
    }
    if (due(kAttitudeMessageId)) {
        json::Value f = json::Value::object();
        f["time_boot_ms"] = static_cast<std::int64_t>(timeSeconds_ * kMavlinkBootTimeScale);
        f["roll"] = roll_;
        f["pitch"] = pitch_;
        f["yaw"] = toRadians(state_.headingDegrees > 180.0 ? state_.headingDegrees - 360.0 : state_.headingDegrees);
        send("ATTITUDE", f);
    }
    if (due(kVfrHudMessageId)) {
        json::Value f = json::Value::object();
        f["groundspeed"] = state_.groundSpeed;
        f["airspeed"] = state_.groundSpeed;
        f["alt"] = options_.homeAltitudeMsl + state_.relativeAltitude;
        f["climb"] = climb_;
        f["heading"] = static_cast<std::int64_t>(std::llround(state_.headingDegrees)) % static_cast<std::int64_t>(kHeadingWrapDegrees);
        f["throttle"] = state_.flying ? 55 : 0;
        send("VFR_HUD", f);
    }
    if (due(kSystemStatusMessageId)) {
        json::Value f = json::Value::object();
        f["voltage_battery"] = static_cast<std::int64_t>(std::llround(10500.0 + state_.batteryPercent * 21.0));
        f["current_battery"] = state_.flying ? 1800 : 50;
        f["battery_remaining"] = static_cast<std::int64_t>(std::llround(state_.batteryPercent));
        send("SYS_STATUS", f);
    }
    if (due(kGpsRawIntMessageId)) {
        json::Value f = json::Value::object();
        f["time_usec"] = static_cast<std::int64_t>(timeSeconds_ * kMavlinkMicrosecondsPerSecond);
        f["fix_type"] = 3;
        f["lat"] = static_cast<std::int64_t>(std::llround(state_.latitude * kMavlinkPositionScale));
        f["lon"] = static_cast<std::int64_t>(std::llround(state_.longitude * kMavlinkPositionScale));
        f["alt"] = static_cast<std::int64_t>(std::llround((options_.homeAltitudeMsl + state_.relativeAltitude) * kMavlinkAltitudeScale));
        f["satellites_visible"] = 14;
        send("GPS_RAW_INT", f);
    }
}

} // namespace gygax::robotics
