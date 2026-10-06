#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gygax/core/json.hpp>
#include <gygax/neuro/network.hpp>
#include <gygax/transport/streams.hpp>

namespace gygax::sim {

inline constexpr std::size_t kDefaultRecordingIntervalSteps = 5;
inline constexpr double kObstacleClearanceMeters = 0.3;

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

struct Pose {
    double x = 0.0;
    double y = 0.0;
    double theta = 0.0;
};

struct Obstacle {
    Vec2 center;
    double radius = 0.5;
};

struct AgentConfig {
    std::string name;
    double radius = 0.25;
    double wheelBase = 0.4;
    double maxWheelSpeed = 1.5;
    Pose start;
};

struct LidarConfig {
    int beams = 32;
    double fovRad = 6.283185307179586;
    double maxRange = 10.0;
    double noiseStd = 0.0;
};

enum class RunTarget { Simulated, RealHardware };

const char* toString(RunTarget target);

struct Telemetry {
    Pose pose;
    std::vector<float> lidar;
    std::uint64_t sequence = 0;
};

class RobotLink {
public:
    virtual ~RobotLink() = default;
    virtual bool sendDrive(const std::string& agent, double left, double right) = 0;
    virtual std::optional<Telemetry> poll(const std::string& agent) = 0;
};

class StreamRobotLink final : public RobotLink {
public:
    StreamRobotLink(std::shared_ptr<comm::Stream> stream, std::string destination);

    bool sendDrive(const std::string& agent, double left, double right) override;
    std::optional<Telemetry> poll(const std::string& agent) override;

private:
    std::shared_ptr<comm::Stream> stream_;
    std::string destination_;
    std::map<std::string, Telemetry> latest_;
};

struct AgentState {
    AgentConfig config;
    Pose pose;
    double linear = 0.0;
    double angular = 0.0;
    double leftCommand = 0.0;
    double rightCommand = 0.0;
    double distance = 0.0;
    std::uint64_t collisions = 0;
    std::uint64_t contactSteps = 0;
    bool inContact = false;
    RunTarget target = RunTarget::Simulated;
    std::optional<Telemetry> remote;
};

class World2D {
public:
    explicit World2D(double width = 20.0, double height = 20.0, std::uint64_t seed = 1);

    std::uint32_t spawn(AgentConfig config);
    void addObstacle(Obstacle obstacle);
    void addRandomObstacles(std::size_t count, double minRadius, double maxRadius, double keepClear = 1.5);

    bool setWheelSpeeds(std::uint32_t handle, double left, double right);
    void step(double dtSeconds);

    [[nodiscard]] std::vector<float> lidar(std::uint32_t handle, const LidarConfig& config);
    [[nodiscard]] std::optional<Pose> pose(std::uint32_t handle) const;
    [[nodiscard]] const AgentState* agent(std::uint32_t handle) const;
    [[nodiscard]] std::size_t agentCount() const { return agents_.size(); }
    [[nodiscard]] const std::vector<Obstacle>& obstacles() const { return obstacles_; }
    [[nodiscard]] double time() const { return time_; }
    [[nodiscard]] double width() const { return width_; }
    [[nodiscard]] double height() const { return height_; }
    [[nodiscard]] std::uint64_t totalCollisions() const;
    [[nodiscard]] bool anyOverlap() const;

    bool setRunTarget(std::uint32_t handle, RunTarget target);
    void attachLink(std::shared_ptr<RobotLink> link) { link_ = std::move(link); }
    [[nodiscard]] RunTarget runTarget(std::uint32_t handle) const;

    void setRecording(bool enabled, std::size_t everyNSteps = kDefaultRecordingIntervalSteps);
    [[nodiscard]] std::string toUsda() const;
    [[nodiscard]] json::Value snapshot() const;

private:
    struct Sample {
        double time;
        std::vector<Pose> poses;
    };

    AgentState* find(std::uint32_t handle);
    void integrate(AgentState& a, double dt);
    void resolve(AgentState& a, std::uint32_t handle);
    [[nodiscard]] double castRay(std::uint32_t self, Vec2 origin, double angle, double maxRange) const;

    double width_;
    double height_;
    neuro::Rng rng_;
    std::uint32_t nextHandle_ = 1;
    std::map<std::uint32_t, AgentState> agents_;
    std::vector<Obstacle> obstacles_;
    double time_ = 0.0;
    std::shared_ptr<RobotLink> link_;
    bool recording_ = false;
    std::size_t recordEvery_ = 5;
    std::size_t stepCounter_ = 0;
    std::vector<Sample> samples_;
};

}
