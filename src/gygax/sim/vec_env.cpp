#include <gygax/sim/vec_env.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace gygax::sim {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinimumEnvironmentWidth = 2.0;
constexpr double kMaximumEnvironmentWidth = 1000.0;
constexpr double kMaximumEnvironmentCount = 16384.0;
constexpr double kMaximumObstacleCount = 500.0;
constexpr double kMinimumObstacleRadius = 0.05;
constexpr double kMaximumObstacleRadius = 50.0;
constexpr double kMaximumBeamCount = 1024.0;
constexpr double kMaximumLidarRange = 1000.0;
constexpr double kMaximumTimeStepSeconds = 1.0;
constexpr double kMaximumEpisodeSteps = 1.0e7;
constexpr double kMaximumWheelSpeed = 100.0;
constexpr double kMaximumWorkerThreads = 256.0;
constexpr double kMaximumSeedValue = 9.0e15;
constexpr double kMaximumActionDelaySteps = 64.0;
constexpr double kMaximumProbability = 1.0;
constexpr double kMinimumWheelBase = 0.05;
constexpr int kGoalPlacementAttempts = 100;
constexpr double kSpawnMarginMeters = 1.0;
constexpr double kProgressRewardScale = 2.0;
constexpr double kStepRewardPenalty = 0.01;
constexpr double kCollisionRewardPenalty = 1.0;
constexpr double kGoalReward = 10.0;

Pose requirePose(World2D& world, std::uint32_t handle) {
    if (const auto p = world.pose(handle)) return *p;
    throw std::logic_error("VecEnv: agent handle vanished from its own world");
}

double draw(neuro::Rng& rng, Range r) {
    return r.lo + (r.hi - r.lo) * rng.uniform();
}

std::uint64_t mix(std::uint64_t seed, std::uint64_t salt) {
    std::uint64_t z = seed + 0x9E3779B97F4A7C15ULL * (salt + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

}

std::optional<VecEnvConfig> vecEnvConfigFromJson(const json::Value& spec, std::string* error) {
    auto fail = [&](const std::string& m) -> std::optional<VecEnvConfig> {
        if (error != nullptr) *error = m;
        return std::nullopt;
    };
    if (!spec.isObject()) return fail("config must be a JSON object");
    VecEnvConfig c;
    auto number = [&](const char* key, double& out, double lo, double hi, std::string& err) {
        const auto* v = spec.find(key);
        if (v == nullptr) return true;
        if (!v->isNumber() || v->asDouble() < lo || v->asDouble() > hi) {
            err = std::string(key) + " must be a number in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]";
            return false;
        }
        out = v->asDouble();
        return true;
    };
    std::string err;
    double numEnvs = static_cast<double>(c.numEnvs);
    double obstacles = static_cast<double>(c.obstacles);
    double beams = c.beams;
    double maxSteps = static_cast<double>(c.maxSteps);
    double threads = static_cast<double>(c.threads);
    double seed = static_cast<double>(c.seed);
    const bool ok = number("num_envs", numEnvs, 1, kMaximumEnvironmentCount, err) &&
                    number("width", c.width, kMinimumEnvironmentWidth, kMaximumEnvironmentWidth, err) &&
                    number("height", c.height, kMinimumEnvironmentWidth, kMaximumEnvironmentWidth, err) &&
                    number("obstacles", obstacles, 0, kMaximumObstacleCount, err) &&
                    number("obstacle_min_radius", c.obstacleMinRadius, kMinimumObstacleRadius, kMaximumObstacleRadius, err) &&
                    number("obstacle_max_radius", c.obstacleMaxRadius, kMinimumObstacleRadius, kMaximumObstacleRadius, err) &&
                    number("beams", beams, 1, kMaximumBeamCount, err) &&
                    number("max_range", c.maxRange, 0.1, kMaximumLidarRange, err) &&
                    number("dt", c.dt, 0.001, kMaximumTimeStepSeconds, err) &&
                    number("max_steps", maxSteps, 1, kMaximumEpisodeSteps, err) &&
                    number("goal_radius", c.goalRadius, kMinimumObstacleRadius, kMaximumObstacleRadius, err) &&
                    number("max_wheel_speed", c.maxWheelSpeed, 0.01, kMaximumWheelSpeed, err) &&
                    number("threads", threads, 1, kMaximumWorkerThreads, err) && number("seed", seed, 0, kMaximumSeedValue, err);
    if (!ok) return fail(err);
    if (c.obstacleMinRadius > c.obstacleMaxRadius) return fail("obstacle_min_radius must not exceed obstacle_max_radius");
    c.numEnvs = static_cast<std::size_t>(numEnvs);
    c.obstacles = static_cast<std::size_t>(obstacles);
    c.beams = static_cast<int>(beams);
    c.maxSteps = static_cast<std::size_t>(maxSteps);
    c.threads = static_cast<std::size_t>(threads);
    c.seed = static_cast<std::uint64_t>(seed);
    if (const auto* v = spec.find("auto_reset")) c.autoReset = v->asBool(true);
    if (const auto* rz = spec.find("randomization")) {
        if (!rz->isObject()) return fail("randomization must be an object");
        struct Entry {
            const char* key;
            Range* range;
        };
        Randomization& r = c.randomization;
        const Entry entries[] = {{"motor_gain", &r.motorGain},          {"wheel_base", &r.wheelBase},
                                 {"lidar_noise_std", &r.lidarNoiseStd}, {"lidar_dropout", &r.lidarDropout},
                                 {"pose_noise_std", &r.poseNoiseStd},   {"action_delay_steps", &r.actionDelaySteps},
                                 {"action_dropout", &r.actionDropout}};
        for (const auto& [key, range] : entries) {
            const auto* v = rz->find(key);
            if (v == nullptr) continue;
            if (!v->isArray() || v->size() != 2 || !v->asArray()[0].isNumber() || !v->asArray()[1].isNumber())
                return fail(std::string("randomization.") + key + " must be [lo, hi]");
            range->lo = v->asArray()[0].asDouble();
            range->hi = v->asArray()[1].asDouble();
            if (range->lo > range->hi || range->lo < 0.0) return fail(std::string("randomization.") + key + " needs 0 <= lo <= hi");
        }
        if (r.actionDelaySteps.hi > kMaximumActionDelaySteps) return fail("randomization.action_delay_steps is limited to 64");
        if (r.lidarDropout.hi > kMaximumProbability || r.actionDropout.hi > kMaximumProbability)
            return fail("dropout probabilities must not exceed 1");
        if (r.wheelBase.lo < kMinimumWheelBase) return fail("randomization.wheel_base must be at least 0.05");
        if (r.motorGain.lo <= 0.0) return fail("randomization.motor_gain must be positive");
    }
    return c;
}

struct VecEnv::Env {
    explicit Env(std::uint64_t seed) : rng(seed) {}

    neuro::Rng rng;
    std::unique_ptr<World2D> world;
    std::uint32_t handle = 0;
    Vec2 goal;
    std::size_t steps = 0;
    double motorGain = 1.0;
    double lidarNoise = 0.0;
    double lidarDropout = 0.0;
    double poseNoise = 0.0;
    double actionDropout = 0.0;
    std::size_t delay = 0;
    std::deque<std::pair<double, double>> queue;
    std::pair<double, double> lastApplied{0.0, 0.0};
    double prevDistance = 0.0;
    std::uint64_t episodeCollisions = 0;
};

VecEnv::VecEnv(const VecEnvConfig& config) : config_(config) {
    if (config_.numEnvs == 0) config_.numEnvs = 1;
    if (config_.beams < 1) config_.beams = 1;
    if (config_.threads == 0) config_.threads = 1;
    const auto n = config_.numEnvs;
    obs_.assign(n * observationSize(), 0.0F);
    finalObs_.assign(n * observationSize(), 0.0F);
    rewards_.assign(n, 0.0F);
    terminated_.assign(n, 0);
    truncated_.assign(n, 0);
    collided_.assign(n, 0);
    reached_.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) envs_.push_back(std::make_unique<Env>(mix(config_.seed, i)));
    reset(config_.seed);
}

VecEnv::~VecEnv() = default;

void VecEnv::reset(std::uint64_t seed) {
    config_.seed = seed;
    for (std::size_t i = 0; i < envs_.size(); ++i) {
        envs_[i]->rng.seed(mix(seed, i));
        resetEnv(i);
    }
    episodes_ = 0;
    steps_ = 0;
}

void VecEnv::resetEnv(std::size_t index) {
    Env& e = *envs_[index];
    const auto& rz = config_.randomization;
    e.motorGain = draw(e.rng, rz.motorGain);
    e.lidarNoise = draw(e.rng, rz.lidarNoiseStd);
    e.lidarDropout = draw(e.rng, rz.lidarDropout);
    e.poseNoise = draw(e.rng, rz.poseNoiseStd);
    e.actionDropout = draw(e.rng, rz.actionDropout);
    e.delay = static_cast<std::size_t>(std::llround(draw(e.rng, rz.actionDelaySteps)));
    e.queue.clear();
    e.lastApplied = {0.0, 0.0};
    e.steps = 0;
    e.episodeCollisions = 0;

    e.world = std::make_unique<World2D>(config_.width, config_.height, e.rng.next());
    AgentConfig cfg;
    cfg.name = "rover";
    cfg.wheelBase = draw(e.rng, rz.wheelBase);
    cfg.maxWheelSpeed = config_.maxWheelSpeed * 2.0;
    const double margin = kSpawnMarginMeters;
    cfg.start = {margin + (config_.width - 2 * margin) * e.rng.uniform(), margin + (config_.height - 2 * margin) * e.rng.uniform(),
                 (e.rng.uniform() * 2.0 - 1.0) * kPi};
    e.handle = e.world->spawn(cfg);
    e.world->addRandomObstacles(config_.obstacles, config_.obstacleMinRadius, config_.obstacleMaxRadius,
                                kObstacleClearanceMeters);

    const double minGoalDistance = std::min(config_.width, config_.height) * 0.3;
    for (int attempt = 0; attempt < kGoalPlacementAttempts; ++attempt) {
        Vec2 g{margin + (config_.width - 2 * margin) * e.rng.uniform(), margin + (config_.height - 2 * margin) * e.rng.uniform()};
        bool ok = std::hypot(g.x - cfg.start.x, g.y - cfg.start.y) >= minGoalDistance;
        for (const auto& o : e.world->obstacles())
            if (std::hypot(g.x - o.center.x, g.y - o.center.y) <
                o.radius + config_.goalRadius + kObstacleClearanceMeters)
                ok = false;
        e.goal = g;
        if (ok) break;
    }
    const auto p = requirePose(*e.world, e.handle);
    e.prevDistance = std::hypot(e.goal.x - p.x, e.goal.y - p.y);
    observe(index, &obs_[index * observationSize()]);
}

void VecEnv::observe(std::size_t index, float* out) {
    Env& e = *envs_[index];
    LidarConfig lc;
    lc.beams = config_.beams;
    lc.maxRange = config_.maxRange;
    lc.noiseStd = e.lidarNoise;
    const auto scan = e.world->lidar(e.handle, lc);
    for (int b = 0; b < config_.beams; ++b) {
        double r = scan[static_cast<std::size_t>(b)];
        if (e.lidarDropout > 0.0 && e.rng.bernoulli(e.lidarDropout)) r = config_.maxRange;
        out[b] = static_cast<float>(r / config_.maxRange);
    }
    auto p = requirePose(*e.world, e.handle);
    if (e.poseNoise > 0.0) {
        p.x += e.poseNoise * e.rng.normal();
        p.y += e.poseNoise * e.rng.normal();
        p.theta += e.poseNoise * e.rng.normal();
    }
    const double dx = e.goal.x - p.x;
    const double dy = e.goal.y - p.y;
    const double diag = std::hypot(config_.width, config_.height);
    const double bearing = std::remainder(std::atan2(dy, dx) - p.theta, 2.0 * kPi);
    const auto* a = e.world->agent(e.handle);
    float* tail = out + config_.beams;
    tail[0] = static_cast<float>(std::hypot(dx, dy) / diag);
    tail[1] = static_cast<float>(bearing / kPi);
    tail[2] = static_cast<float>(a->linear / config_.maxWheelSpeed);
    tail[3] = static_cast<float>(a->angular / (2.0 * config_.maxWheelSpeed / a->config.wheelBase));
}

void VecEnv::stepOne(std::size_t index, const float* action) {
    Env& e = *envs_[index];
    const auto* self = e.world->agent(e.handle);
    const double lin = std::clamp(static_cast<double>(action[0]), -1.0, 1.0);
    const double ang = std::clamp(static_cast<double>(action[1]), -1.0, 1.0);
    std::pair<double, double> cmd{lin - ang, lin + ang};
    cmd.first = std::clamp(cmd.first, -1.0, 1.0) * config_.maxWheelSpeed;
    cmd.second = std::clamp(cmd.second, -1.0, 1.0) * config_.maxWheelSpeed;
    e.queue.push_back(cmd);
    std::pair<double, double> applied = e.lastApplied;
    if (e.queue.size() > e.delay) {
        applied = e.queue.front();
        e.queue.pop_front();
    }
    if (e.actionDropout > 0.0 && e.rng.bernoulli(e.actionDropout)) applied = e.lastApplied;
    e.lastApplied = applied;
    e.world->setWheelSpeeds(e.handle, applied.first * e.motorGain, applied.second * e.motorGain);
    const auto before = self->collisions;
    e.world->step(config_.dt);
    ++e.steps;

    const auto p = requirePose(*e.world, e.handle);
    const double distance = std::hypot(e.goal.x - p.x, e.goal.y - p.y);
    const bool hit = e.world->agent(e.handle)->collisions > before;
    const bool reached = distance < config_.goalRadius;
    if (hit) ++e.episodeCollisions;
    double reward = (e.prevDistance - distance) * kProgressRewardScale - kStepRewardPenalty;
    if (hit) reward -= kCollisionRewardPenalty;
    if (reached) reward += kGoalReward;
    e.prevDistance = distance;
    rewards_[index] = static_cast<float>(reward);
    collided_[index] = hit ? 1 : 0;
    reached_[index] = reached ? 1 : 0;
    terminated_[index] = reached ? 1 : 0;
    truncated_[index] = (!reached && e.steps >= config_.maxSteps) ? 1 : 0;
    observe(index, &obs_[index * observationSize()]);
    if (terminated_[index] || truncated_[index]) {
        std::copy_n(&obs_[index * observationSize()], observationSize(), &finalObs_[index * observationSize()]);
        if (config_.autoReset) resetEnv(index);
    }
}

void VecEnv::step(const std::vector<float>& actions) {
    step(actions.data());
}

void VecEnv::step(const float* actions) {
    const std::size_t n = envs_.size();
    const std::size_t workers = std::min(config_.threads, n);
    if (workers <= 1) {
        for (std::size_t i = 0; i < n; ++i) stepOne(i, actions + i * kDifferentialDriveActionSize);
    } else {
        std::vector<std::thread> pool;
        const std::size_t per = (n + workers - 1) / workers;
        for (std::size_t w = 0; w < workers; ++w) {
            const std::size_t lo = w * per;
            const std::size_t hi = std::min(n, lo + per);
            if (lo >= hi) break;
            pool.emplace_back([this, lo, hi, actions] {
                for (std::size_t i = lo; i < hi; ++i) stepOne(i, actions + i * kDifferentialDriveActionSize);
            });
        }
        for (auto& t : pool) t.join();
    }
    steps_ += n;
    for (std::size_t i = 0; i < n; ++i)
        if (terminated_[i] || truncated_[i]) ++episodes_;
}

}
