#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gygax/core/json.hpp>
#include <gygax/neuro/network.hpp>
#include <gygax/sim/world2d.hpp>

namespace gygax::sim {

inline constexpr std::size_t kNonLidarObservationFeatures = 4;
inline constexpr std::size_t kDifferentialDriveActionSize = 2;

struct Range {
    double lo = 0.0;
    double hi = 0.0;
};

struct Randomization {
    Range motorGain{1.0, 1.0};
    Range wheelBase{0.4, 0.4};
    Range lidarNoiseStd{0.0, 0.0};
    Range lidarDropout{0.0, 0.0};
    Range poseNoiseStd{0.0, 0.0};
    Range actionDelaySteps{0.0, 0.0};
    Range actionDropout{0.0, 0.0};
};

struct VecEnvConfig {
    std::size_t numEnvs = 8;
    double width = 12.0;
    double height = 12.0;
    std::size_t obstacles = 6;
    double obstacleMinRadius = 0.3;
    double obstacleMaxRadius = 0.8;
    int beams = 16;
    double maxRange = 6.0;
    double dt = 0.05;
    std::size_t maxSteps = 400;
    double goalRadius = 0.4;
    double maxWheelSpeed = 1.5;
    std::uint64_t seed = 1;
    std::size_t threads = 1;
    bool autoReset = true;
    Randomization randomization;
};

std::optional<VecEnvConfig> vecEnvConfigFromJson(const json::Value& spec, std::string* error);

class VecEnv {
public:
    explicit VecEnv(const VecEnvConfig& config);
    ~VecEnv();
    VecEnv(const VecEnv&) = delete;
    VecEnv& operator=(const VecEnv&) = delete;

    [[nodiscard]] std::size_t numEnvs() const { return config_.numEnvs; }
    [[nodiscard]] std::size_t observationSize() const {
        return static_cast<std::size_t>(config_.beams) + kNonLidarObservationFeatures;
    }
    [[nodiscard]] std::size_t actionSize() const { return kDifferentialDriveActionSize; }
    [[nodiscard]] const VecEnvConfig& config() const { return config_; }

    void reset(std::uint64_t seed);
    void resetEnv(std::size_t index);
    void step(const float* actions);
    void step(const std::vector<float>& actions);

    [[nodiscard]] const std::vector<float>& observations() const { return obs_; }
    [[nodiscard]] const std::vector<float>& finalObservations() const { return finalObs_; }
    [[nodiscard]] const std::vector<float>& rewards() const { return rewards_; }
    [[nodiscard]] const std::vector<std::uint8_t>& terminated() const { return terminated_; }
    [[nodiscard]] const std::vector<std::uint8_t>& truncated() const { return truncated_; }
    [[nodiscard]] const std::vector<std::uint8_t>& collided() const { return collided_; }
    [[nodiscard]] const std::vector<std::uint8_t>& reachedGoal() const { return reached_; }
    [[nodiscard]] std::uint64_t episodesFinished() const { return episodes_; }
    [[nodiscard]] std::uint64_t totalSteps() const { return steps_; }

private:
    struct Env;
    void stepOne(std::size_t index, const float* action);
    void observe(std::size_t index, float* out);

    VecEnvConfig config_;
    std::vector<std::unique_ptr<Env>> envs_;
    std::vector<float> obs_;
    std::vector<float> finalObs_;
    std::vector<float> rewards_;
    std::vector<std::uint8_t> terminated_;
    std::vector<std::uint8_t> truncated_;
    std::vector<std::uint8_t> collided_;
    std::vector<std::uint8_t> reached_;
    std::uint64_t episodes_ = 0;
    std::uint64_t steps_ = 0;
};

}
