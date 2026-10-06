#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace gygax::robotics {

/**
 * Robotics-side command filters and freshness helpers.
 *
 * These are software components, not certified safety functions, hardware
 * interlocks, or human-approval gates. Integrators remain responsible for
 * independent limits, watchdogs, emergency stops, and operator authorization.
 */

inline constexpr double kNoTargetTimestamp = -1e300;

struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

struct CircleObstacle {
    Point2 center;
    double radius = 0.5;
};

struct ShieldConfig {
    double alpha = 2.0;
    double margin = 0.1;
    double maxSpeed = 1.0;
    double bodyRadius = 0.25;
    std::optional<Point2> boundsMin;
    std::optional<Point2> boundsMax;
};

struct ShieldResult {
    Point2 velocity;
    bool modified = false;
    bool infeasible = false;
    std::size_t activeConstraints = 0;
};

class VelocityShield {
public:
    /**
     * Configure a velocity filter; invalid limits, obstacles, or non-finite
     * inputs throw std::invalid_argument. Infeasible constraints require
     * caller action; this class is not a certified safety function.
     */
    explicit VelocityShield(const ShieldConfig& config = {});

    void setObstacles(std::vector<CircleObstacle> obstacles);
    void addObstacle(CircleObstacle obstacle);
    [[nodiscard]] const ShieldConfig& config() const { return config_; }

    /** Return a filtered command and report whether constraints were infeasible. */
    [[nodiscard]] ShieldResult filter(Point2 position, Point2 desired) const;
    [[nodiscard]] ShieldResult filterAlongHeading(Point2 position, double heading, double desiredSpeed) const;

private:
    struct Constraint {
        Point2 normal;
        double bound;
    };
    [[nodiscard]] std::vector<Constraint> constraints(Point2 position) const;

    ShieldConfig config_;
    std::vector<CircleObstacle> obstacles_;
};

struct JointLimit {
    double min = -3.14159265358979;
    double max = 3.14159265358979;
    double maxVelocity = 1.0;
    double maxAcceleration = 10.0;
};

class JointShield {
public:
    /** Invalid joint limits, reset vectors, or non-positive timesteps throw std::invalid_argument. */
    explicit JointShield(std::vector<JointLimit> limits);

    void reset(const std::vector<double>& position);
    [[nodiscard]] std::vector<double> filter(const std::vector<double>& commandedPosition, double dt);
    [[nodiscard]] std::size_t clippedCount() const { return clipped_; }

private:
    std::vector<JointLimit> limits_;
    std::vector<double> position_;
    std::vector<double> velocity_;
    std::size_t clipped_ = 0;
};

class ActionChunkEnsembler {
public:
    explicit ActionChunkEnsembler(double decay = 0.01, std::size_t maxChunks = 64);

    void push(std::size_t startStep, std::vector<std::vector<double>> chunk);
    [[nodiscard]] std::optional<std::vector<double>> action(std::size_t step);
    [[nodiscard]] std::size_t pending() const { return chunks_.size(); }
    void clear() { chunks_.clear(); }

private:
    struct Chunk {
        std::size_t start;
        std::vector<std::vector<double>> actions;
    };
    double decay_;
    std::size_t maxChunks_;
    std::deque<Chunk> chunks_;
};

class TargetHold {
public:
    /** Requires a positive finite timeout and finite fallback/target values. */
    explicit TargetHold(double timeoutSeconds, std::vector<double> fallback);

    void publish(std::vector<double> target, double nowSeconds);
    [[nodiscard]] std::vector<double> latest(double nowSeconds) const;
    [[nodiscard]] bool stale(double nowSeconds) const;
    [[nodiscard]] std::size_t publishCount() const { return published_; }

private:
    double timeout_;
    std::vector<double> fallback_;
    std::vector<double> target_;
    double stamp_ = kNoTargetTimestamp;
    std::size_t published_ = 0;
};

}
