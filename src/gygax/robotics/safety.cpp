#include <gygax/robotics/safety.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gygax::robotics {

namespace {

constexpr int kMaximumShieldProjectionPasses = 64;
constexpr double kConstraintProjectionTolerance = 1e-12;
constexpr double kMinimumConstraintNormalSquared = 1e-18;
constexpr double kConstraintFeasibilityTolerance = 1e-6;
constexpr double kActiveConstraintTolerance = 1e-6;
constexpr double kVelocityModificationTolerance = 1e-9;
constexpr double kJointPositionTolerance = 1e-12;

void requireFinite(double value, const char* name) {
    if (!std::isfinite(value)) throw std::invalid_argument(name);
}

void validatePoint(Point2 point, const char* name) {
    requireFinite(point.x, name);
    requireFinite(point.y, name);
}

void validateObstacle(const CircleObstacle& obstacle) {
    validatePoint(obstacle.center, "obstacle center must be finite");
    requireFinite(obstacle.radius, "obstacle radius must be finite");
    if (obstacle.radius < 0.0) throw std::invalid_argument("obstacle radius must be nonnegative");
}

void validateShieldConfig(const ShieldConfig& config) {
    requireFinite(config.alpha, "shield alpha must be finite");
    requireFinite(config.margin, "shield margin must be finite");
    requireFinite(config.maxSpeed, "shield maximum speed must be finite");
    requireFinite(config.bodyRadius, "shield body radius must be finite");
    if (config.alpha <= 0.0 || config.margin < 0.0 || config.maxSpeed <= 0.0 || config.bodyRadius < 0.0)
        throw std::invalid_argument("shield alpha and maximum speed must be positive; margin and body radius nonnegative");
    if (config.boundsMin) validatePoint(*config.boundsMin, "minimum bounds must be finite");
    if (config.boundsMax) validatePoint(*config.boundsMax, "maximum bounds must be finite");
    if (config.boundsMin && config.boundsMax && (config.boundsMin->x >= config.boundsMax->x || config.boundsMin->y >= config.boundsMax->y))
        throw std::invalid_argument("minimum bounds must be below maximum bounds on both axes");
}

}

VelocityShield::VelocityShield(const ShieldConfig& config) : config_(config) {
    validateShieldConfig(config_);
}

void VelocityShield::setObstacles(std::vector<CircleObstacle> obstacles) {
    for (const auto& obstacle : obstacles) validateObstacle(obstacle);
    obstacles_ = std::move(obstacles);
}

void VelocityShield::addObstacle(CircleObstacle obstacle) {
    validateObstacle(obstacle);
    obstacles_.push_back(obstacle);
}

std::vector<VelocityShield::Constraint> VelocityShield::constraints(Point2 p) const {
    std::vector<Constraint> out;
    for (const auto& o : obstacles_) {
        const double dx = p.x - o.center.x;
        const double dy = p.y - o.center.y;
        const double safe = o.radius + config_.bodyRadius + config_.margin;
        const double h = dx * dx + dy * dy - safe * safe;
        out.push_back({{2.0 * dx, 2.0 * dy}, -config_.alpha * h});
    }
    if (config_.boundsMin) {
        const double lo = config_.bodyRadius + config_.margin;
        out.push_back({{1.0, 0.0}, -config_.alpha * (p.x - config_.boundsMin->x - lo)});
        out.push_back({{0.0, 1.0}, -config_.alpha * (p.y - config_.boundsMin->y - lo)});
    }
    if (config_.boundsMax) {
        const double lo = config_.bodyRadius + config_.margin;
        out.push_back({{-1.0, 0.0}, -config_.alpha * (config_.boundsMax->x - p.x - lo)});
        out.push_back({{0.0, -1.0}, -config_.alpha * (config_.boundsMax->y - p.y - lo)});
    }
    return out;
}

ShieldResult VelocityShield::filter(Point2 position, Point2 desired) const {
    validatePoint(position, "position must be finite");
    validatePoint(desired, "desired velocity must be finite");
    ShieldResult r;
    Point2 u = desired;
    const auto cs = constraints(position);
    auto clampSpeed = [&](Point2& v) {
        const double s = std::hypot(v.x, v.y);
        if (s > config_.maxSpeed) {
            v.x *= config_.maxSpeed / s;
            v.y *= config_.maxSpeed / s;
        }
    };
    clampSpeed(u);
    for (int pass = 0; pass < kMaximumShieldProjectionPasses; ++pass) {
        bool changed = false;
        for (const auto& c : cs) {
            const double lhs = c.normal.x * u.x + c.normal.y * u.y;
            if (lhs >= c.bound - kConstraintProjectionTolerance) continue;
            const double n2 = c.normal.x * c.normal.x + c.normal.y * c.normal.y;
            if (n2 < kMinimumConstraintNormalSquared) continue;
            const double k = (c.bound - lhs) / n2;
            u.x += k * c.normal.x;
            u.y += k * c.normal.y;
            changed = true;
        }
        clampSpeed(u);
        if (!changed) break;
    }
    for (const auto& c : cs) {
        const double lhs = c.normal.x * u.x + c.normal.y * u.y;
        if (lhs < c.bound - kConstraintFeasibilityTolerance) r.infeasible = true;
        if (std::fabs(lhs - c.bound) < kActiveConstraintTolerance) ++r.activeConstraints;
    }
    r.velocity = u;
    r.modified = std::hypot(u.x - desired.x, u.y - desired.y) > kVelocityModificationTolerance;
    return r;
}

ShieldResult VelocityShield::filterAlongHeading(Point2 position, double heading, double desiredSpeed) const {
    validatePoint(position, "position must be finite");
    requireFinite(heading, "heading must be finite");
    requireFinite(desiredSpeed, "desired speed must be finite");
    ShieldResult r;
    const Point2 h{std::cos(heading), std::sin(heading)};
    double lo = -config_.maxSpeed;
    double hi = config_.maxSpeed;
    for (const auto& c : constraints(position)) {
        const double a = c.normal.x * h.x + c.normal.y * h.y;
        if (std::fabs(a) < 1e-12) {
            if (c.bound > 0.0) r.infeasible = true;
            continue;
        }
        const double v = c.bound / a;
        if (a > 0.0)
            lo = std::max(lo, v);
        else
            hi = std::min(hi, v);
    }
    double speed = std::clamp(desiredSpeed, -config_.maxSpeed, config_.maxSpeed);
    if (lo > hi) {
        r.infeasible = true;
        speed = 0.0;
    } else {
        speed = std::clamp(speed, lo, hi);
    }
    r.velocity = {h.x * speed, h.y * speed};
    r.modified = std::fabs(speed - desiredSpeed) > kVelocityModificationTolerance;
    return r;
}

JointShield::JointShield(std::vector<JointLimit> limits)
    : limits_(std::move(limits)), position_(limits_.size(), 0.0), velocity_(limits_.size(), 0.0) {
    for (const auto& limit : limits_) {
        requireFinite(limit.min, "joint minimum must be finite");
        requireFinite(limit.max, "joint maximum must be finite");
        requireFinite(limit.maxVelocity, "joint maximum velocity must be finite");
        requireFinite(limit.maxAcceleration, "joint maximum acceleration must be finite");
        if (limit.min >= limit.max || limit.maxVelocity < 0.0 || limit.maxAcceleration < 0.0)
            throw std::invalid_argument("joint minimum must be below maximum; velocity and acceleration limits must be nonnegative");
    }
}

void JointShield::reset(const std::vector<double>& position) {
    if (position.size() != position_.size()) throw std::invalid_argument("joint reset position count does not match configured limits");
    for (std::size_t i = 0; i < position_.size(); ++i) {
        requireFinite(position[i], "joint reset positions must be finite");
        position_[i] = std::clamp(position[i], limits_[i].min, limits_[i].max);
        velocity_[i] = 0.0;
    }
}

std::vector<double> JointShield::filter(const std::vector<double>& commanded, double dt) {
    requireFinite(dt, "joint filter timestep must be finite");
    if (dt <= 0.0) throw std::invalid_argument("joint filter timestep must be positive");
    std::vector<double> out(position_.size());
    for (std::size_t i = 0; i < position_.size(); ++i) {
        const auto& l = limits_[i];
        double target = i < commanded.size() ? commanded[i] : position_[i];
        if (!std::isfinite(target)) target = position_[i];
        double v = (target - position_[i]) / dt;
        double vClamped = std::clamp(v, -l.maxVelocity, l.maxVelocity);
        const double toMax = std::max(0.0, l.max - position_[i]);
        const double toMin = std::max(0.0, position_[i] - l.min);
        const double brake = l.maxAcceleration * dt;
        vClamped = std::min(vClamped, std::max(0.0, std::sqrt(2.0 * l.maxAcceleration * toMax + brake * brake / 4.0) - brake / 2.0));
        vClamped = std::max(vClamped, -std::max(0.0, std::sqrt(2.0 * l.maxAcceleration * toMin + brake * brake / 4.0) - brake / 2.0));
        const double dvMax = l.maxAcceleration * dt;
        vClamped = std::clamp(vClamped, velocity_[i] - dvMax, velocity_[i] + dvMax);
        double next = position_[i] + vClamped * dt;
        if (next > l.max || next < l.min) {
            next = std::clamp(next, l.min, l.max);
            vClamped = (next - position_[i]) / dt;
        }
        if (std::fabs(next - target) > kJointPositionTolerance) ++clipped_;
        velocity_[i] = vClamped;
        position_[i] = next;
        out[i] = next;
    }
    return out;
}

ActionChunkEnsembler::ActionChunkEnsembler(double decay, std::size_t maxChunks) : decay_(decay), maxChunks_(maxChunks) {}

void ActionChunkEnsembler::push(std::size_t startStep, std::vector<std::vector<double>> chunk) {
    if (chunk.empty()) return;
    chunks_.push_back({startStep, std::move(chunk)});
    while (chunks_.size() > maxChunks_) chunks_.pop_front();
}

std::optional<std::vector<double>> ActionChunkEnsembler::action(std::size_t step) {
    while (!chunks_.empty() && chunks_.front().start + chunks_.front().actions.size() <= step) chunks_.pop_front();
    std::vector<double> sum;
    double total = 0.0;
    std::size_t rank = 0;
    for (const auto& c : chunks_) {
        if (step < c.start || step >= c.start + c.actions.size()) continue;
        const auto& a = c.actions[step - c.start];
        if (sum.empty()) sum.assign(a.size(), 0.0);
        if (a.size() != sum.size()) continue;
        const double w = std::exp(-decay_ * static_cast<double>(rank++));
        for (std::size_t i = 0; i < a.size(); ++i) sum[i] += w * a[i];
        total += w;
    }
    if (sum.empty() || total <= 0.0) return std::nullopt;
    for (auto& v : sum) v /= total;
    return sum;
}

TargetHold::TargetHold(double timeoutSeconds, std::vector<double> fallback) : timeout_(timeoutSeconds), fallback_(std::move(fallback)) {
    requireFinite(timeout_, "target timeout must be finite");
    if (timeout_ <= 0.0) throw std::invalid_argument("target timeout must be positive");
    for (const double value : fallback_) requireFinite(value, "fallback target values must be finite");
}

void TargetHold::publish(std::vector<double> target, double nowSeconds) {
    requireFinite(nowSeconds, "target timestamp must be finite");
    if (target.size() != fallback_.size()) throw std::invalid_argument("target dimension must match fallback dimension");
    for (const double value : target) requireFinite(value, "target values must be finite");
    target_ = std::move(target);
    stamp_ = nowSeconds;
    ++published_;
}

bool TargetHold::stale(double nowSeconds) const {
    return target_.empty() || !std::isfinite(nowSeconds) || nowSeconds < stamp_ || nowSeconds - stamp_ > timeout_;
}

std::vector<double> TargetHold::latest(double nowSeconds) const {
    return stale(nowSeconds) ? fallback_ : target_;
}

}
