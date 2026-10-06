#include <gygax/sim/world2d.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <sstream>

namespace gygax::sim {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kObstaclePlacementAttemptsPerObstacle = 200;
constexpr double kAngularVelocityEpsilon = 1e-9;
constexpr double kRayIntersectionEpsilon = 1e-12;
constexpr double kIntegrationStepSeconds = 0.01;
constexpr double kFiniteDifferenceEpsilon = 1e-6;

double wrapAngle(double a) {
    while (a > kPi) a -= 2.0 * kPi;
    while (a < -kPi) a += 2.0 * kPi;
    return a;
}

std::optional<double> rayCircle(Vec2 o, double angle, Vec2 c, double r) {
    const double dx = std::cos(angle);
    const double dy = std::sin(angle);
    const double fx = o.x - c.x;
    const double fy = o.y - c.y;
    const double b = fx * dx + fy * dy;
    const double cc = fx * fx + fy * fy - r * r;
    const double disc = b * b - cc;
    if (disc < 0.0) return std::nullopt;
    const double root = std::sqrt(disc);
    const double t1 = -b - root;
    const double t2 = -b + root;
    if (t1 >= 0.0) return t1;
    if (t2 >= 0.0) return 0.0;
    return std::nullopt;
}

}

const char* toString(RunTarget target) {
    return target == RunTarget::Simulated ? "Simulated" : "RealHardware";
}

StreamRobotLink::StreamRobotLink(std::shared_ptr<comm::Stream> stream, std::string destination)
    : stream_(std::move(stream)), destination_(std::move(destination)) {}

bool StreamRobotLink::sendDrive(const std::string& agent, double left, double right) {
    json::Value msg = json::Value::object();
    msg["agent"] = agent;
    msg["cmd"] = "drive";
    msg["left"] = left;
    msg["right"] = right;
    return stream_->transmit(destination_, msg.dump());
}

std::optional<Telemetry> StreamRobotLink::poll(const std::string& agent) {
    while (auto line = stream_->receive()) {
        auto parsed = json::parse(*line);
        if (!parsed) continue;
        const json::Value& doc = *parsed;
        const auto name = doc.getString("agent");
        if (name.empty()) continue;
        Telemetry t;
        if (const auto* pose = doc.find("pose")) {
            t.pose.x = pose->getDouble("x");
            t.pose.y = pose->getDouble("y");
            t.pose.theta = pose->getDouble("theta");
        }
        if (const auto* lidar = doc.find("lidar")) {
            t.lidar.reserve(lidar->size());
            for (const auto& v : lidar->asArray()) t.lidar.push_back(static_cast<float>(v.asDouble()));
        }
        t.sequence = static_cast<std::uint64_t>(doc.getInt("seq"));
        latest_[name] = std::move(t);
    }
    auto it = latest_.find(agent);
    if (it == latest_.end()) return std::nullopt;
    return it->second;
}

World2D::World2D(double width, double height, std::uint64_t seed) : width_(width), height_(height), rng_(seed) {}

std::uint32_t World2D::spawn(AgentConfig config) {
    AgentState state;
    state.pose = config.start;
    state.config = std::move(config);
    const auto handle = nextHandle_++;
    agents_.emplace(handle, std::move(state));
    return handle;
}

void World2D::addObstacle(Obstacle obstacle) {
    obstacles_.push_back(obstacle);
}

void World2D::addRandomObstacles(std::size_t count, double minRadius, double maxRadius, double keepClear) {
    std::size_t placed = 0;
    std::size_t attempts = 0;
    while (placed < count && attempts++ < count * kObstaclePlacementAttemptsPerObstacle) {
        Obstacle o;
        o.radius = minRadius + (maxRadius - minRadius) * rng_.uniform();
        o.center.x = o.radius + (width_ - 2.0 * o.radius) * rng_.uniform();
        o.center.y = o.radius + (height_ - 2.0 * o.radius) * rng_.uniform();
        bool ok = true;
        for (const auto& [h, a] : agents_) {
            if (std::hypot(o.center.x - a.pose.x, o.center.y - a.pose.y) < o.radius + a.config.radius + keepClear) ok = false;
        }
        for (const auto& other : obstacles_) {
            if (std::hypot(o.center.x - other.center.x, o.center.y - other.center.y) <
                o.radius + other.radius + kObstacleClearanceMeters)
                ok = false;
        }
        if (ok) {
            obstacles_.push_back(o);
            ++placed;
        }
    }
}

AgentState* World2D::find(std::uint32_t handle) {
    auto it = agents_.find(handle);
    return it == agents_.end() ? nullptr : &it->second;
}

const AgentState* World2D::agent(std::uint32_t handle) const {
    auto it = agents_.find(handle);
    return it == agents_.end() ? nullptr : &it->second;
}

bool World2D::setWheelSpeeds(std::uint32_t handle, double left, double right) {
    auto* a = find(handle);
    if (a == nullptr) return false;
    const double limit = a->config.maxWheelSpeed;
    a->leftCommand = std::clamp(left, -limit, limit);
    a->rightCommand = std::clamp(right, -limit, limit);
    if (a->target == RunTarget::RealHardware && link_) return link_->sendDrive(a->config.name, a->leftCommand, a->rightCommand);
    return true;
}

void World2D::integrate(AgentState& a, double dt) {
    const double v = (a.leftCommand + a.rightCommand) / 2.0;
    const double w = (a.rightCommand - a.leftCommand) / a.config.wheelBase;
    const double x0 = a.pose.x;
    const double y0 = a.pose.y;
    if (std::fabs(w) < kAngularVelocityEpsilon) {
        a.pose.x += v * std::cos(a.pose.theta) * dt;
        a.pose.y += v * std::sin(a.pose.theta) * dt;
    } else {
        const double t1 = a.pose.theta + w * dt;
        a.pose.x += v / w * (std::sin(t1) - std::sin(a.pose.theta));
        a.pose.y += v / w * (std::cos(a.pose.theta) - std::cos(t1));
    }
    a.pose.theta = wrapAngle(a.pose.theta + w * dt);
    a.linear = v;
    a.angular = w;
    a.distance += std::hypot(a.pose.x - x0, a.pose.y - y0);
}

void World2D::resolve(AgentState& a, std::uint32_t handle) {
    bool hit = false;
    const double r = a.config.radius;
    if (a.pose.x < r) {
        a.pose.x = r;
        hit = true;
    }
    if (a.pose.x > width_ - r) {
        a.pose.x = width_ - r;
        hit = true;
    }
    if (a.pose.y < r) {
        a.pose.y = r;
        hit = true;
    }
    if (a.pose.y > height_ - r) {
        a.pose.y = height_ - r;
        hit = true;
    }
    auto push = [&](Vec2 c, double cr) {
        const double dx = a.pose.x - c.x;
        const double dy = a.pose.y - c.y;
        const double dist = std::hypot(dx, dy);
        const double minDist = r + cr;
        if (dist < minDist) {
            const double nx = dist > 1e-9 ? dx / dist : 1.0;
            const double ny = dist > 1e-9 ? dy / dist : 0.0;
            a.pose.x = c.x + nx * minDist;
            a.pose.y = c.y + ny * minDist;
            hit = true;
        }
    };
    for (const auto& o : obstacles_) push(o.center, o.radius);
    for (const auto& [other, state] : agents_) {
        if (other != handle) push({state.pose.x, state.pose.y}, state.config.radius);
    }
    if (hit) {
        ++a.contactSteps;
        if (!a.inContact) ++a.collisions;
    }
    a.inContact = hit;
}

void World2D::step(double dt) {
    if (dt <= 0.0) return;
    const int substeps = std::max(1, static_cast<int>(std::ceil(dt / kIntegrationStepSeconds)));
    const double stepSize = dt / substeps;
    for (int s = 0; s < substeps; ++s) {
        for (auto& [handle, a] : agents_) {
            if (a.target == RunTarget::RealHardware) continue;
            integrate(a, stepSize);
            resolve(a, handle);
        }
    }
    for (auto& [handle, a] : agents_) {
        if (a.target != RunTarget::RealHardware || !link_) continue;
        if (auto t = link_->poll(a.config.name)) {
            a.remote = *t;
            a.pose = t->pose;
        }
    }
    time_ += dt;
    if (recording_ && ++stepCounter_ % recordEvery_ == 0) {
        Sample sample;
        sample.time = time_;
        for (const auto& [h, a] : agents_) sample.poses.push_back(a.pose);
        samples_.push_back(std::move(sample));
    }
}

double World2D::castRay(std::uint32_t self, Vec2 origin, double angle, double maxRange) const {
    double best = maxRange;
    for (const auto& o : obstacles_) {
        if (auto t = rayCircle(origin, angle, o.center, o.radius)) best = std::min(best, *t);
    }
    for (const auto& [handle, a] : agents_) {
        if (handle == self) continue;
        if (auto t = rayCircle(origin, angle, {a.pose.x, a.pose.y}, a.config.radius)) best = std::min(best, *t);
    }
    const double dx = std::cos(angle);
    const double dy = std::sin(angle);
    if (dx > 1e-12) best = std::min(best, (width_ - origin.x) / dx);
    if (dx < -kRayIntersectionEpsilon) best = std::min(best, (0.0 - origin.x) / dx);
    if (dy > 1e-12) best = std::min(best, (height_ - origin.y) / dy);
    if (dy < -kRayIntersectionEpsilon) best = std::min(best, (0.0 - origin.y) / dy);
    return std::max(0.0, best);
}

std::vector<float> World2D::lidar(std::uint32_t handle, const LidarConfig& config) {
    std::vector<float> out;
    const auto* a = agent(handle);
    if (a == nullptr || config.beams <= 0) return out;
    if (a->target == RunTarget::RealHardware && a->remote && !a->remote->lidar.empty()) return a->remote->lidar;
    out.reserve(static_cast<std::size_t>(config.beams));
    for (int i = 0; i < config.beams; ++i) {
        const double frac = config.beams == 1 ? 0.5 : static_cast<double>(i) / (config.beams - 1);
        const double angle = a->pose.theta - config.fovRad / 2.0 + frac * config.fovRad;
        double range = castRay(handle, {a->pose.x, a->pose.y}, angle, config.maxRange);
        if (config.noiseStd > 0.0) range = std::clamp(range + config.noiseStd * rng_.normal(), 0.0, config.maxRange);
        out.push_back(static_cast<float>(range));
    }
    return out;
}

std::optional<Pose> World2D::pose(std::uint32_t handle) const {
    const auto* a = agent(handle);
    if (a == nullptr) return std::nullopt;
    return a->pose;
}

std::uint64_t World2D::totalCollisions() const {
    std::uint64_t total = 0;
    for (const auto& [h, a] : agents_) total += a.collisions;
    return total;
}

bool World2D::anyOverlap() const {
    constexpr double eps = kFiniteDifferenceEpsilon;
    for (const auto& [h, a] : agents_) {
        if (a.target == RunTarget::RealHardware) continue;
        for (const auto& o : obstacles_) {
            if (std::hypot(a.pose.x - o.center.x, a.pose.y - o.center.y) < a.config.radius + o.radius - eps) return true;
        }
        for (const auto& [h2, b] : agents_) {
            if (h2 <= h || b.target == RunTarget::RealHardware) continue;
            if (std::hypot(a.pose.x - b.pose.x, a.pose.y - b.pose.y) < a.config.radius + b.config.radius - eps) return true;
        }
    }
    return false;
}

bool World2D::setRunTarget(std::uint32_t handle, RunTarget target) {
    auto* a = find(handle);
    if (a == nullptr) return false;
    if (target == RunTarget::RealHardware && !link_) return false;
    a->target = target;
    if (target == RunTarget::RealHardware) {
        a->remote.reset();
        return link_->sendDrive(a->config.name, a->leftCommand, a->rightCommand);
    }
    return true;
}

RunTarget World2D::runTarget(std::uint32_t handle) const {
    const auto* a = agent(handle);
    return a == nullptr ? RunTarget::Simulated : a->target;
}

void World2D::setRecording(bool enabled, std::size_t everyNSteps) {
    recording_ = enabled;
    recordEvery_ = std::max<std::size_t>(1, everyNSteps);
    if (!enabled) samples_.clear();
}

json::Value World2D::snapshot() const {
    json::Value out = json::Value::object();
    out["time"] = time_;
    out["width"] = width_;
    out["height"] = height_;
    json::Value agents = json::Value::array();
    for (const auto& [h, a] : agents_) {
        json::Value item = json::Value::object();
        item["handle"] = h;
        item["name"] = a.config.name;
        item["x"] = a.pose.x;
        item["y"] = a.pose.y;
        item["theta"] = a.pose.theta;
        item["distance"] = a.distance;
        item["collisions"] = a.collisions;
        item["contact_steps"] = a.contactSteps;
        item["target"] = toString(a.target);
        agents.push(std::move(item));
    }
    out["agents"] = std::move(agents);
    return out;
}

std::string World2D::toUsda() const {
    std::ostringstream out;
    const bool animated = !samples_.empty();
    out << "#usda 1.0\n(\n    defaultPrim = \"World\"\n    metersPerUnit = 1\n    upAxis = \"Z\"\n";
    if (animated) {
        out << "    startTimeCode = 0\n    endTimeCode = " << samples_.size() << "\n    timeCodesPerSecond = "
            << (samples_.size() > 1 ? std::max(1.0, std::round(static_cast<double>(samples_.size()) / std::max(time_, 1e-9))) : 1.0)
            << "\n";
    }
    out << ")\n\ndef Xform \"World\"\n{\n";
    out << std::format("    def Cube \"Floor\"\n    {{\n        double size = 1\n        double3 xformOp:scale = ({}, {}, 0.02)\n"
                       "        double3 xformOp:translate = ({}, {}, -0.01)\n        uniform token[] xformOpOrder = "
                       "[\"xformOp:translate\", \"xformOp:scale\"]\n    }}\n",
                       width_, height_, width_ / 2.0, height_ / 2.0);
    std::size_t index = 0;
    for (const auto& o : obstacles_) {
        out << std::format(
            "    def Cylinder \"Obstacle_{}\"\n    {{\n        double height = 1\n        double radius = {}\n"
            "        double3 xformOp:translate = ({}, {}, 0.5)\n        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n    }}\n",
            index++, o.radius, o.center.x, o.center.y);
    }
    std::size_t agentIndex = 0;
    for (const auto& [handle, a] : agents_) {
        out << std::format("    def Xform \"{}\"\n    {{\n", a.config.name);
        if (animated) {
            out << "        double3 xformOp:translate.timeSamples = {\n";
            for (std::size_t s = 0; s < samples_.size(); ++s) {
                out << std::format("            {}: ({}, {}, {}),\n", s + 1, samples_[s].poses[agentIndex].x,
                                   samples_[s].poses[agentIndex].y, a.config.radius);
            }
            out << "        }\n        float xformOp:rotateZ.timeSamples = {\n";
            for (std::size_t s = 0; s < samples_.size(); ++s) {
                out << std::format("            {}: {},\n", s + 1, samples_[s].poses[agentIndex].theta * 180.0 / kPi);
            }
            out << "        }\n";
        } else {
            out << std::format("        double3 xformOp:translate = ({}, {}, {})\n        float xformOp:rotateZ = {}\n", a.pose.x, a.pose.y,
                               a.config.radius, a.pose.theta * 180.0 / kPi);
        }
        out << "        uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:rotateZ\"]\n";
        out << std::format(
            "        def Cylinder \"Body\"\n        {{\n            double height = {}\n            double radius = {}\n        }}\n",
            a.config.radius, a.config.radius);
        out << "    }\n";
        ++agentIndex;
    }
    out << "}\n";
    return out.str();
}

}
