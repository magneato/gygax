module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <gygax/brain/taxonomy.hpp>
#include <gygax/core/agent.hpp>
#include <gygax/core/json.hpp>
#include <gygax/core/log.hpp>
#include <gygax/transport/streams.hpp>

import gygax.world_model;

namespace gygax::taxonomy {
namespace {
constexpr double kDegreesToRadians = 3.14159265358979 / 180.0;
constexpr float kMaximumControlPercentage = 100.0f;
constexpr float kVehicleMaximumSteeringAngleDegrees = 35.0f;
constexpr double kVehicleAccelerationMetersPerSecondSquared = 4.0;
constexpr double kVehicleBrakingMetersPerSecondSquared = 9.0;
constexpr double kVehicleSpeedDragPerSecond = 0.05;
constexpr double kVehicleWheelbaseMeters = 2.7;
constexpr float kVesselMaximumRudderAngleDegrees = 40.0f;
constexpr double kVesselMaximumSpeedMetersPerSecond = 8.0;
constexpr double kVesselSpeedResponsePerSecond = 0.4;
constexpr double kVesselTurnRateScale = 0.05;
constexpr float kUavMaximumAttitudeDegrees = 60.0f;
constexpr double kUavMaximumVerticalSpeedMetersPerSecond = 5.0;
constexpr float kDroneMaximumAttitudeDegrees = 45.0f;
constexpr float kDroneTakeoffAltitudeMeters = 10.0f;
constexpr double kDroneMaximumVerticalSpeedMetersPerSecond = 3.0;
constexpr double kDroneLandingAltitudeToleranceMeters = 0.01;
constexpr float kInitialBallastKilograms = 50.0f;
constexpr float kInitialLiftingGasLiters = 10000.0f;
constexpr float kOrbitalDegreesPerTurn = 360.0f;
constexpr float kCameraMinimumZoom = 1.0f;
constexpr float kCameraMaximumZoom = 30.0f;
constexpr int kCameraMinimumBitrateKbps = 64;
constexpr int kCameraMaximumBitrateKbps = 50000;
constexpr int kCameraDefaultBitrateKbps = 4000;
}
}
import gygax.messaging;
import gygax.tools;
import gygax.core.base;

export module gygax.taxonomy.base;

namespace gygax::taxonomy {

template <typename T> class AgentBase : public T, public BaseObject {
public:
    AgentBase(std::string name, uint32_t id, gygax::Capability caps) : BaseObject(std::move(name), caps), sid_(id) {}

    ~AgentBase() override { stopWorker(); }

    uint32_t sid() const override { return sid_; }

    void attachStream(std::shared_ptr<comm::Stream> stream) override {
        std::lock_guard lock(mutex_);
        const auto key = stream->name();
        streams_[key] = std::move(stream);
    }

    [[nodiscard]] std::shared_ptr<comm::Stream> getStream(const std::string& streamName) const override {
        std::lock_guard lock(mutex_);
        if (auto it = streams_.find(streamName); it != streams_.end()) return it->second;
        return nullptr;
    }

    bool receiveMessage(const messaging::Message& msg) override {
        {
            std::lock_guard lock(mutex_);
            inbox_.push_back(msg.payload);
            if (inbox_.size() > kInboxLimit) inbox_.erase(inbox_.begin());
        }
        wake_.notify_all();
        return true;
    }

    void emitEvent(uint32_t id, const void*, size_t size) override {
        std::lock_guard lock(mutex_);
        ++eventsEmitted_;
        lastEvent_ = std::format("{}:{}", id, size);
    }

    [[nodiscard]] std::vector<std::string> drainInbox() {
        std::lock_guard lock(mutex_);
        return std::exchange(inbox_, {});
    }

    void initialize() override {
        BaseObject::initialize();
        startWorker();
    }

    void freeze(bool f) override {
        frozen_.store(f, std::memory_order_release);
        BaseObject::freeze(f);
    }

    void shutdown() override {
        stopWorker();
        BaseObject::shutdown();
    }

    void pre_delete() override { BaseObject::pre_delete(); }
    void reset() override { BaseObject::reset(); }
    Capability capabilities() const override { return BaseObject::capabilities(); }
    const std::string& name() const override { return BaseObject::name(); }
    bool compatible(Capability r) const override { return BaseObject::compatible(r); }
    bool compatible(const Object& o) const override { return BaseObject::compatible(o); }
    std::string diff(const Object& o) const override { return BaseObject::diff(o); }

    void advance(double dtSeconds) override {
        std::lock_guard lock(mutex_);
        ++ticks_;
        integrate(dtSeconds);
    }

    [[nodiscard]] std::string telemetry() const override {
        std::lock_guard lock(mutex_);
        json::Value out = json::Value::object();
        out["name"] = name_;
        out["sid"] = sid_;
        out["ticks"] = ticks_;
        out["events_emitted"] = eventsEmitted_;
        out["inbox"] = inbox_.size();
        out["frozen"] = frozen_.load();
        describe(out);
        return out.dump();
    }

protected:
    virtual void integrate(double) {}
    virtual void describe(json::Value&) const {}

    uint32_t sid_;
    mutable std::mutex mutex_;

private:
    static constexpr std::size_t kInboxLimit = 1024;
    static constexpr auto kPeriod = std::chrono::milliseconds(20);

    void startWorker() {
        if (worker_.joinable()) return;
        worker_ = std::jthread([this](std::stop_token st) {
            auto last = std::chrono::steady_clock::now();
            while (!st.stop_requested()) {
                {
                    std::unique_lock lock(waitMutex_);
                    wake_.wait_for(lock, st, kPeriod, [] { return false; });
                }
                const auto now = std::chrono::steady_clock::now();
                const double dt = std::chrono::duration<double>(now - last).count();
                last = now;
                if (!frozen_.load(std::memory_order_acquire)) advance(dt);
            }
        });
    }

    void stopWorker() {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
    }

    std::jthread worker_;
    std::mutex waitMutex_;
    std::condition_variable_any wake_;
    std::atomic<bool> frozen_{false};
    std::unordered_map<std::string, std::shared_ptr<comm::Stream>> streams_;
    std::vector<std::string> inbox_;
    uint64_t ticks_ = 0;
    uint64_t eventsEmitted_ = 0;
    std::string lastEvent_;
};

namespace Computer {
class CapabilityImpl : public AgentBase<Computer::Capability> {
public:
    static constexpr Type type = Type::Capability;
    static constexpr const char* name = "Capability";

    explicit CapabilityImpl(uint32_t id) : AgentBase("CapabilityNode", id, gygax::Capability::Cognition) {}

    void runTask(const std::string& script) override {
        const auto space = script.find(' ');
        const std::string tool = script.substr(0, space);
        const std::string input = space == std::string::npos ? std::string() : script.substr(space + 1);
        std::string outcome;
        try {
            auto result = tools::ToolRegistry::getInstance().execute(tool, input);
            outcome = result ? *result : "error: unknown tool '" + tool + "'";
        } catch (const std::exception& e) {
            outcome = std::string("error: ") + e.what();
        }
        std::lock_guard lock(mutex_);
        lastTask_ = script;
        lastResult_ = std::move(outcome);
        ++tasks_;
    }

private:
    void describe(json::Value& out) const override {
        out["tasks"] = tasks_;
        out["last_task"] = lastTask_;
        out["last_result"] = lastResult_;
    }

    std::string lastTask_;
    std::string lastResult_;
    uint64_t tasks_ = 0;
};
FACTORY_REGISTER(CapabilityImpl);
}

namespace Mobility {
class VehicleImpl : public AgentBase<Mobility::Vehicle> {
public:
    static constexpr Type type = Type::Vehicle;
    static constexpr const char* name = "Vehicle";

    explicit VehicleImpl(uint32_t id) : AgentBase("VehicleNode", id, Capability::Locomotion | Capability::Actuation) {}

    void action(float steerAngle, float throttlePct, float brakeForce) override {
        steer(steerAngle);
        setThrottle(throttlePct);
        applyBraking(brakeForce);
    }
    void steer(float angle) override {
        std::lock_guard lock(mutex_);
        steer_ = std::clamp(angle, -kVehicleMaximumSteeringAngleDegrees, kVehicleMaximumSteeringAngleDegrees);
    }
    void setThrottle(float pct) override {
        std::lock_guard lock(mutex_);
        throttle_ = std::clamp(pct, 0.0f, kMaximumControlPercentage);
    }
    void applyBraking(float force) override {
        std::lock_guard lock(mutex_);
        brake_ = std::clamp(force, 0.0f, kMaximumControlPercentage);
    }

private:
    void integrate(double dt) override {
        const double accel = throttle_ / kMaximumControlPercentage * kVehicleAccelerationMetersPerSecondSquared -
                             brake_ / kMaximumControlPercentage * kVehicleBrakingMetersPerSecondSquared -
                             kVehicleSpeedDragPerSecond * speed_;
        speed_ = std::max(0.0, speed_ + accel * dt);
        heading_ += speed_ / kVehicleWheelbaseMeters * std::tan(steer_ * kDegreesToRadians) * dt;
        x_ += speed_ * std::cos(heading_) * dt;
        y_ += speed_ * std::sin(heading_) * dt;
    }

    void describe(json::Value& out) const override {
        out["steer_deg"] = steer_;
        out["throttle_pct"] = throttle_;
        out["brake_pct"] = brake_;
        out["speed_mps"] = speed_;
        out["heading_rad"] = heading_;
        out["x"] = x_;
        out["y"] = y_;
    }

    float steer_ = 0;
    float throttle_ = 0;
    float brake_ = 0;
    double speed_ = 0;
    double heading_ = 0;
    double x_ = 0;
    double y_ = 0;
};
FACTORY_REGISTER(VehicleImpl);
}

namespace Marine {
class VesselImpl : public AgentBase<Marine::Vessel> {
public:
    static constexpr Type type = Type::Vessel;
    static constexpr const char* name = "Vessel";

    explicit VesselImpl(uint32_t id) : AgentBase("VesselNode", id, Capability::Locomotion | Capability::Actuation) {}

    void action(float rudder, float thrust, float) override {
        setRudder(rudder);
        setPropulsion(thrust);
    }
    void setRudder(float angle) override {
        std::lock_guard lock(mutex_);
        rudder_ = std::clamp(angle, -kVesselMaximumRudderAngleDegrees, kVesselMaximumRudderAngleDegrees);
    }
    void setPropulsion(float thrust) override {
        std::lock_guard lock(mutex_);
        thrust_ = std::clamp(thrust, -kMaximumControlPercentage, kMaximumControlPercentage);
    }

private:
    void integrate(double dt) override {
        const double target = thrust_ / kMaximumControlPercentage * kVesselMaximumSpeedMetersPerSecond;
        speed_ += (target - speed_) * std::min(1.0, kVesselSpeedResponsePerSecond * dt);
        heading_ += speed_ * std::sin(rudder_ * kDegreesToRadians) * kVesselTurnRateScale * dt;
        x_ += speed_ * std::cos(heading_) * dt;
        y_ += speed_ * std::sin(heading_) * dt;
    }

    void describe(json::Value& out) const override {
        out["rudder_deg"] = rudder_;
        out["thrust_pct"] = thrust_;
        out["speed_mps"] = speed_;
        out["heading_rad"] = heading_;
        out["x"] = x_;
        out["y"] = y_;
    }

    float rudder_ = 0;
    float thrust_ = 0;
    double speed_ = 0;
    double heading_ = 0;
    double x_ = 0;
    double y_ = 0;
};
FACTORY_REGISTER(VesselImpl);
}

namespace Aerospace {
class UAVImpl : public AgentBase<Aerospace::UAV> {
public:
    static constexpr Type type = Type::UAV;
    static constexpr const char* name = "UAV";

    explicit UAVImpl(uint32_t id) : AgentBase("UAVNode", id, Capability::Locomotion | Capability::Actuation) {}

    void action(float pitch, float roll, float yaw) override { setFlightPath(pitch, roll, yaw); }
    void setFlightPath(float pitch, float roll, float yaw) override {
        std::lock_guard lock(mutex_);
        pitch_ = std::clamp(pitch, -kUavMaximumAttitudeDegrees, kUavMaximumAttitudeDegrees);
        roll_ = std::clamp(roll, -kUavMaximumAttitudeDegrees, kUavMaximumAttitudeDegrees);
        yaw_ = yaw;
    }
    void setAltitude(float meters) override {
        std::lock_guard lock(mutex_);
        targetAltitude_ = std::max(0.0f, meters);
    }

private:
    void integrate(double dt) override {
        const double delta = targetAltitude_ - altitude_;
        const double step = kUavMaximumVerticalSpeedMetersPerSecond * dt;
        altitude_ += std::clamp(delta, -step, step);
    }

    void describe(json::Value& out) const override {
        out["pitch_deg"] = pitch_;
        out["roll_deg"] = roll_;
        out["yaw_deg"] = yaw_;
        out["altitude_m"] = altitude_;
        out["target_altitude_m"] = targetAltitude_;
    }

    float pitch_ = 0;
    float roll_ = 0;
    float yaw_ = 0;
    double altitude_ = 0;
    float targetAltitude_ = 0;
};
FACTORY_REGISTER(UAVImpl);

class DroneImpl : public AgentBase<Aerospace::Drone> {
public:
    static constexpr Type type = Type::Drone;
    static constexpr const char* name = "Drone";

    explicit DroneImpl(uint32_t id) : AgentBase("DroneNode", id, Capability::Locomotion | Capability::Actuation | Capability::Perception) {}

    void action(float pitch, float roll, float yaw) override { setFlightPath(pitch, roll, yaw); }
    void setFlightPath(float pitch, float roll, float yaw) override {
        std::lock_guard lock(mutex_);
        pitch_ = std::clamp(pitch, -kDroneMaximumAttitudeDegrees, kDroneMaximumAttitudeDegrees);
        roll_ = std::clamp(roll, -kDroneMaximumAttitudeDegrees, kDroneMaximumAttitudeDegrees);
        yaw_ = yaw;
    }
    void setAltitude(float meters) override {
        std::lock_guard lock(mutex_);
        if (mode_ == Mode::Grounded) return;
        targetAltitude_ = std::max(0.0f, meters);
        mode_ = Mode::Flying;
    }
    void takeoff() override {
        std::lock_guard lock(mutex_);
        if (mode_ != Mode::Grounded) return;
        targetAltitude_ = kDroneTakeoffAltitudeMeters;
        mode_ = Mode::Flying;
    }
    void land() override {
        std::lock_guard lock(mutex_);
        if (mode_ == Mode::Grounded) return;
        targetAltitude_ = 0.0f;
        mode_ = Mode::Landing;
    }
    void hover() override {
        std::lock_guard lock(mutex_);
        if (mode_ == Mode::Grounded) return;
        targetAltitude_ = static_cast<float>(altitude_);
        mode_ = Mode::Hovering;
    }

private:
    enum class Mode { Grounded, Flying, Hovering, Landing };

    void integrate(double dt) override {
        const double delta = targetAltitude_ - altitude_;
        const double step = kDroneMaximumVerticalSpeedMetersPerSecond * dt;
        altitude_ += std::clamp(delta, -step, step);
        if (mode_ == Mode::Landing && altitude_ <= kDroneLandingAltitudeToleranceMeters) {
            altitude_ = 0.0;
            mode_ = Mode::Grounded;
        }
    }

    void describe(json::Value& out) const override {
        static constexpr const char* modes[] = {"grounded", "flying", "hovering", "landing"};
        out["mode"] = modes[static_cast<int>(mode_)];
        out["altitude_m"] = altitude_;
        out["target_altitude_m"] = targetAltitude_;
        out["pitch_deg"] = pitch_;
        out["roll_deg"] = roll_;
        out["yaw_deg"] = yaw_;
    }

    Mode mode_ = Mode::Grounded;
    float pitch_ = 0;
    float roll_ = 0;
    float yaw_ = 0;
    double altitude_ = 0;
    float targetAltitude_ = 0;
};
FACTORY_REGISTER(DroneImpl);

class HighAltitudePlatformImpl : public AgentBase<Aerospace::HighAltitudePlatform> {
public:
    static constexpr Type type = Type::HighAltitudePlatform;
    static constexpr const char* name = "HighAltitudePlatform";

    explicit HighAltitudePlatformImpl(uint32_t id) : AgentBase("HighAltitudeNode", id, Capability::Locomotion | Capability::Perception) {}

    void action(float ballastKg, float ventLitres, float) override {
        adjustBallast(ballastKg);
        ventGas(ventLitres);
    }
    void adjustBallast(float mass) override {
        std::lock_guard lock(mutex_);
        ballastKg_ = std::max(0.0f, ballastKg_ - std::max(0.0f, mass));
        ballastDroppedKg_ += std::max(0.0f, mass);
    }
    void ventGas(float volume) override {
        std::lock_guard lock(mutex_);
        gasLitres_ = std::max(0.0f, gasLitres_ - std::max(0.0f, volume));
        gasVentedLitres_ += std::max(0.0f, volume);
    }

private:
    void describe(json::Value& out) const override {
        out["ballast_kg"] = ballastKg_;
        out["gas_litres"] = gasLitres_;
        out["ballast_dropped_kg"] = ballastDroppedKg_;
        out["gas_vented_litres"] = gasVentedLitres_;
    }

    float ballastKg_ = kInitialBallastKilograms;
    float gasLitres_ = kInitialLiftingGasLiters;
    float ballastDroppedKg_ = 0.0f;
    float gasVentedLitres_ = 0.0f;
};
FACTORY_REGISTER(HighAltitudePlatformImpl);
}

namespace Space {
class OrbitalConstellationImpl : public AgentBase<Space::OrbitalConstellation> {
public:
    static constexpr Type type = Type::OrbitalConstellation;
    static constexpr const char* name = "OrbitalConstellation";

    explicit OrbitalConstellationImpl(uint32_t id) : AgentBase("OrbitalSatNode", id, Capability::Perception | Capability::Actuation) {}

    void action(float panelAngle, float duration, float magnitude) override {
        orientSolarPanels(panelAngle);
        pulseThrusters(duration, magnitude);
    }
    void orientSolarPanels(float angle) override {
        std::lock_guard lock(mutex_);
        panelAngle_ = std::fmod(angle, kOrbitalDegreesPerTurn);
    }
    void pulseThrusters(float duration, float magnitude) override {
        if (duration <= 0.0f || magnitude <= 0.0f) return;
        std::lock_guard lock(mutex_);
        deltaV_ += static_cast<double>(duration) * magnitude;
        ++pulses_;
    }

private:
    void describe(json::Value& out) const override {
        out["panel_angle_deg"] = panelAngle_;
        out["delta_v"] = deltaV_;
        out["pulses"] = pulses_;
    }

    float panelAngle_ = 0;
    double deltaV_ = 0;
    uint64_t pulses_ = 0;
};
FACTORY_REGISTER(OrbitalConstellationImpl);
}

namespace Infrastructure {
class StationaryTrafficMonitorImpl : public AgentBase<Infrastructure::StationaryTrafficMonitor> {
public:
    static constexpr Type type = Type::StationaryTrafficMonitor;
    static constexpr const char* name = "StationaryTrafficMonitor";

    explicit StationaryTrafficMonitorImpl(uint32_t id) : AgentBase("TrafficCamNode", id, Capability::Perception) {}

    void action(float pan, float tilt, float zoom) override { panTiltZoom(pan, tilt, zoom); }
    void panTiltZoom(float pan, float tilt, float zoom) override {
        std::lock_guard lock(mutex_);
        pan_ = std::clamp(pan, -170.0f, 170.0f);
        tilt_ = std::clamp(tilt, -30.0f, 90.0f);
        zoom_ = std::clamp(zoom, kCameraMinimumZoom, kCameraMaximumZoom);
    }
    void encodeVideo(int bitrate) override {
        std::lock_guard lock(mutex_);
        bitrateKbps_ = std::clamp(bitrate, kCameraMinimumBitrateKbps, kCameraMaximumBitrateKbps);
    }

private:
    void describe(json::Value& out) const override {
        out["pan_deg"] = pan_;
        out["tilt_deg"] = tilt_;
        out["zoom"] = zoom_;
        out["bitrate_kbps"] = bitrateKbps_;
    }

    float pan_ = 0;
    float tilt_ = 0;
    float zoom_ = 1;
    int bitrateKbps_ = kCameraDefaultBitrateKbps;
};
FACTORY_REGISTER(StationaryTrafficMonitorImpl);
}

namespace Edge {
class MobileEdgeDeviceImpl : public AgentBase<Edge::MobileEdgeDevice> {
public:
    static constexpr Type type = Type::MobileEdgeDevice;
    static constexpr const char* name = "MobileEdgeDevice";

    explicit MobileEdgeDeviceImpl(uint32_t id) : AgentBase("MobilePhoneNode", id, Capability::Perception | Capability::Cognition) {}

    void action(float intensity, float, float) override { vibrate(intensity); }
    void requestComputeOffload(size_t loadBytes) override {
        std::lock_guard lock(mutex_);
        offloadedBytes_ += loadBytes;
        ++offloads_;
    }
    void vibrate(float intensity) override {
        std::lock_guard lock(mutex_);
        vibration_ = std::clamp(intensity, 0.0f, 1.0f);
    }

private:
    void describe(json::Value& out) const override {
        out["offloaded_bytes"] = offloadedBytes_;
        out["offloads"] = offloads_;
        out["vibration"] = vibration_;
    }

    uint64_t offloadedBytes_ = 0;
    uint64_t offloads_ = 0;
    float vibration_ = 0;
};
FACTORY_REGISTER(MobileEdgeDeviceImpl);
}

}

namespace gygax::taxonomy {

extern "C++" {

void ensure_linked() noexcept {}

std::unique_ptr<Computer::Capability> CreateCapabilityModule(uint32_t id) {
    return Create<Computer::Capability>(id);
}
std::unique_ptr<Mobility::Vehicle> CreateVehicleModule(uint32_t id) {
    return Create<Mobility::Vehicle>(id);
}
std::unique_ptr<Marine::Vessel> CreateVesselModule(uint32_t id) {
    return Create<Marine::Vessel>(id);
}
std::unique_ptr<Aerospace::UAV> CreateUAVModule(uint32_t id) {
    return Create<Aerospace::UAV>(id);
}
std::unique_ptr<Aerospace::Drone> CreateDroneModule(uint32_t id) {
    return Create<Aerospace::Drone>(id);
}
std::unique_ptr<Aerospace::HighAltitudePlatform> CreateHighAltitudePlatformModule(uint32_t id) {
    return Create<Aerospace::HighAltitudePlatform>(id);
}
std::unique_ptr<Space::OrbitalConstellation> CreateOrbitalConstellationModule(uint32_t id) {
    return Create<Space::OrbitalConstellation>(id);
}
std::unique_ptr<Infrastructure::StationaryTrafficMonitor> CreateStationaryTrafficMonitorModule(uint32_t id) {
    return Create<Infrastructure::StationaryTrafficMonitor>(id);
}
std::unique_ptr<Edge::MobileEdgeDevice> CreateMobileEdgeDeviceModule(uint32_t id) {
    return Create<Edge::MobileEdgeDevice>(id);
}
}

}
