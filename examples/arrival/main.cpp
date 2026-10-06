#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gygax/core/json.hpp>
#include <gygax/core/log.hpp>
#include <gygax/sim/world2d.hpp>
#include <gygax/transport/streams.hpp>

import gygax.core.base;
import gygax.core.messaging;
import gygax.hardware.core;
import gygax.hardware.brains;

using namespace gygax;
using namespace gygax::hardware;

namespace {

constexpr double kStepSeconds = 0.02;
constexpr double kMillisecondsPerSecond = 1000.0;
constexpr std::uint16_t kRobotPort = 47401;
constexpr std::uint16_t kHostPort = 47402;
constexpr int kLidarBeamCount = 32;
constexpr double kLidarFieldOfViewRadians = 3.14159265358979;
constexpr double kLidarMaxRangeMeters = 6.0;
constexpr auto kRobotLoopInterval = std::chrono::milliseconds(10);
constexpr auto kRobotStartupDelay = std::chrono::milliseconds(100);

struct Options {
    double seconds = 30.0;
    double realSeconds = 3.0;
    std::string usdPath = "arrival.usda";
    std::uint64_t seed = 7;
    bool promote = true;
};

Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--seconds")
            o.seconds = std::strtod(next().c_str(), nullptr);
        else if (a == "--real-seconds")
            o.realSeconds = std::strtod(next().c_str(), nullptr);
        else if (a == "--usd")
            o.usdPath = next();
        else if (a == "--seed")
            o.seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--no-real")
            o.promote = false;
    }
    return o;
}

class Coordinator : public BaseObject {
public:
    Coordinator() : BaseObject("Coordinator", Capability::Communication) {}

    void onMessage(const std::string& sender, const std::string& msg) override {
        ++received_[sender];
        lastMessage_ = msg;
    }

    [[nodiscard]] std::size_t reports(const std::string& sender) const {
        auto it = received_.find(sender);
        return it == received_.end() ? 0 : it->second;
    }

    [[nodiscard]] std::size_t totalReports() const {
        std::size_t n = 0;
        for (const auto& [k, v] : received_) n += v;
        return n;
    }

private:
    std::map<std::string, std::size_t> received_;
    std::string lastMessage_;
};

class Rover : public Node {
public:
    Rover(std::string name, sim::World2D& world, std::uint32_t handle)
        : Node(std::move(name), Capability::Perception | Capability::Actuation | Capability::Locomotion), world_(world), handle_(handle) {
        addInterface("LidarOut", Medium::Data_Ethernet);
        addInterface("DriveIn", Medium::Electrical);
    }

    void step(double dtMs) override {
        Node::step(dtMs);
        const auto ranges = world_.lidar(handle_, lidarConfig());
        if (ranges.empty()) return;
        double left = 0.0;
        double right = 0.0;
        drive(ranges, dtMs, left, right);
        world_.setWheelSpeeds(handle_, left, right);
        if (blocked_ && !wasBlocked_) sendMessage("Coordinator", "obstacle ahead");
        wasBlocked_ = blocked_;
        elapsedMs_ += dtMs;
        if (elapsedMs_ >= kMillisecondsPerSecond) {
            elapsedMs_ = 0.0;
            if (const auto pose = world_.pose(handle_)) sendMessage("Coordinator", std::format("position {:.1f},{:.1f}", pose->x, pose->y));
        }
    }

    [[nodiscard]] std::uint32_t handle() const { return handle_; }
    [[nodiscard]] virtual std::string controller() const { return "reactive"; }

protected:
    static sim::LidarConfig lidarConfig() {
        sim::LidarConfig c;
        c.beams = kLidarBeamCount;
        c.fovRad = kLidarFieldOfViewRadians;
        c.maxRange = kLidarMaxRangeMeters;
        return c;
    }

    static void sectors(const std::vector<float>& ranges, double& leftMin, double& rightMin, double& frontMin) {
        const std::size_t n = ranges.size();
        leftMin = rightMin = frontMin = 1e9;
        for (std::size_t i = 0; i < n; ++i) {
            const double frac = static_cast<double>(i) / static_cast<double>(n - 1);
            if (frac > 0.5 + 0.08)
                leftMin = std::min<double>(leftMin, ranges[i]);
            else if (frac < 0.5 - 0.08)
                rightMin = std::min<double>(rightMin, ranges[i]);
            else
                frontMin = std::min<double>(frontMin, ranges[i]);
        }
    }

    virtual void drive(const std::vector<float>& ranges, double, double& left, double& right) {
        double l = 0.0;
        double r = 0.0;
        double f = 0.0;
        sectors(ranges, l, r, f);
        const double nearest = std::min({l, r, f});
        blocked_ = nearest < 1.0;
        if (nearest < 1.0) {
            const double turn = l > r ? 1.0 : -1.0;
            left = -0.6 * turn;
            right = 0.6 * turn;
        } else if (nearest < 2.0) {
            const double bias = (l > r ? 1.0 : -1.0) * (2.0 - nearest) * 0.4;
            left = 1.0 - bias;
            right = 1.0 + bias;
        } else {
            left = right = 1.2;
        }
    }

    sim::World2D& world_;
    std::uint32_t handle_;
    bool blocked_ = false;
    bool wasBlocked_ = false;
    double elapsedMs_ = 0.0;
};

class SpikingRover : public Rover {
public:
    SpikingRover(std::string name, sim::World2D& world, std::uint32_t handle)
        : Rover(std::move(name), world, handle), brain_("SNN", 0.5, 11) {
        std::string error;
        const auto spec = json::parse(R"({
          "dt": 0.5, "seed": 11,
          "populations": [
            {"name": "sensors", "type": "poisson", "n": 2, "rate": 0},
            {"name": "motors", "type": "lif", "n": 2, "bias": 22.0}
          ],
          "projections": [
            {"pre": "sensors", "post": "motors", "connect": {"type": "one_to_one"}, "weight": 9.0, "delay_ms": 1.0}
          ]})")
                              .value();
        if (!brain_.configure(spec, &error)) throw std::runtime_error(error);
        brain_.network().connectExplicit(0, 1, {0, 1}, {1, 0}, {-4.0F, -4.0F}, 1.0);
        brain_.bindInput("sensors");
        brain_.bindOutput("motors");
    }

    [[nodiscard]] std::string controller() const override { return "spiking (LIF Braitenberg)"; }
    [[nodiscard]] std::uint64_t spikes() const { return brain_.network().spikeCount(1); }

protected:
    void drive(const std::vector<float>& ranges, double dtMs, double& left, double& right) override {
        double l = 0.0;
        double r = 0.0;
        double f = 0.0;
        sectors(ranges, l, r, f);
        blocked_ = std::min({l, r, f}) < 1.0;
        const auto proximity = [](double range) { return std::clamp((3.0 - range) / 3.0, 0.0, 1.0); };
        brain_.encode({proximity(std::min(l, f)), proximity(std::min(r, f))}, 400.0);
        brain_.clearOutput();
        brain_.step(dtMs);
        const auto counts = brain_.readout();
        const double window = dtMs / kMillisecondsPerSecond;
        const double rateLeft = counts.empty() ? 0.0 : counts[0] / window;
        const double rateRight = counts.size() > 1 ? counts[1] / window : 0.0;
        smoothLeft_ = 0.85 * smoothLeft_ + 0.15 * rateLeft;
        smoothRight_ = 0.85 * smoothRight_ + 0.15 * rateRight;
        left = std::clamp(0.1 + smoothLeft_ / 80.0, -1.5, 1.5);
        right = std::clamp(0.1 + smoothRight_ / 80.0, -1.5, 1.5);
    }

private:
    NeuromorphicNode brain_;
    double smoothLeft_ = 0.0;
    double smoothRight_ = 0.0;
};

class RobotEmulator {
public:
    RobotEmulator(const sim::World2D& arena, std::string replyTo)
        : world_(arena.width(), arena.height(), 99), replyTo_(std::move(replyTo)) {
        for (const auto& o : arena.obstacles()) world_.addObstacle(o);
        stream_ = comm::CreateUdpStream(kRobotPort);
    }

    void start() {
        running_.store(true);
        thread_ = std::thread([this] { loop(); });
    }

    void stop() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

    ~RobotEmulator() { stop(); }

    [[nodiscard]] std::uint64_t commandsReceived() const { return commands_.load(); }

private:
    void loop() {
        std::uint32_t handle = 0;
        std::string name;
        std::uint64_t seq = 0;
        auto last = std::chrono::steady_clock::now();
        while (running_.load()) {
            while (auto msg = stream_->receive()) {
                auto parsed = json::parse(*msg);
                if (!parsed) continue;
                const auto cmd = parsed->getString("cmd");
                if (cmd == "spawn") {
                    sim::AgentConfig cfg;
                    cfg.name = parsed->getString("agent");
                    cfg.start = {parsed->getDouble("x"), parsed->getDouble("y"), parsed->getDouble("theta")};
                    name = cfg.name;
                    handle = world_.spawn(cfg);
                } else if (cmd == "drive" && handle != 0) {
                    world_.setWheelSpeeds(handle, parsed->getDouble("left"), parsed->getDouble("right"));
                    ++commands_;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            if (handle != 0) {
                world_.step(dt);
                json::Value t = json::Value::object();
                t["agent"] = name;
                t["seq"] = ++seq;
                const auto pose = *world_.pose(handle);
                t["pose"]["x"] = pose.x;
                t["pose"]["y"] = pose.y;
                t["pose"]["theta"] = pose.theta;
                json::Value ranges = json::Value::array();
                sim::LidarConfig lc;
                lc.beams = kLidarBeamCount;
                lc.fovRad = kLidarFieldOfViewRadians;
                lc.maxRange = kLidarMaxRangeMeters;
                for (const float v : world_.lidar(handle, lc)) ranges.push(static_cast<double>(v));
                t["lidar"] = std::move(ranges);
                (void)stream_->transmit(replyTo_, t.dump());
            }
            std::this_thread::sleep_for(kRobotLoopInterval);
        }
    }

    sim::World2D world_;
    std::string replyTo_;
    std::unique_ptr<comm::Stream> stream_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> commands_{0};
    std::thread thread_;
};

bool check(bool ok, const std::string& what) {
    std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << "\n";
    return ok;
}

}

int main(int argc, char** argv) {
    const Options opt = parseOptions(argc, argv);
    log::setLevel(log::Level::Warn);

    std::cout << "=== Arrival: multi-agent orchestration with a sim-to-real bridge ===\n";
    sim::World2D world(24.0, 24.0, opt.seed);
    world.setRecording(true, 5);

    std::vector<std::unique_ptr<Rover>> fleet;
    const sim::Pose starts[] = {{3, 3, 0.0}, {3, 21, -0.6}, {21, 3, 2.4}, {21, 21, 3.9}};
    for (int i = 0; i < 4; ++i) {
        sim::AgentConfig cfg;
        cfg.name = "Rover_" + std::to_string(i);
        cfg.start = starts[i];
        const auto handle = world.spawn(cfg);
        if (i == 3)
            fleet.push_back(std::make_unique<SpikingRover>(cfg.name, world, handle));
        else
            fleet.push_back(std::make_unique<Rover>(cfg.name, world, handle));
    }
    world.addRandomObstacles(14, 0.5, 1.2, 2.0);

    Coordinator coordinator;
    coordinator.initialize();
    for (auto& r : fleet) r->initialize();

    std::cout << "\n[phase 1] simulated fleet: " << fleet.size() << " rovers, " << world.obstacles().size() << " obstacles, " << opt.seconds
              << " s of simulated time\n";
    const int steps = static_cast<int>(opt.seconds / kStepSeconds);
    for (int i = 0; i < steps; ++i) {
        for (auto& r : fleet) r->step(kStepSeconds * kMillisecondsPerSecond);
        world.step(kStepSeconds);
    }
    for (const auto& r : fleet) {
        const auto* a = world.agent(r->handle());
        std::cout << "  " << r->name() << "  controller=" << r->controller() << "  distance=" << a->distance
                  << " m  collisions=" << a->collisions << "  target=" << sim::toString(a->target) << "\n";
    }
    bool ok = true;
    for (const auto& r : fleet)
        ok &= check(world.agent(r->handle())->distance > 0.3 * opt.seconds, r->name() + " sustained at least 0.3 m/s");
    ok &= check(!world.anyOverlap(), "no agent overlaps an obstacle or another agent");
    ok &= check(coordinator.totalReports() > 0, "coordinator received rover reports through the message hub");
    const auto* spiker = dynamic_cast<SpikingRover*>(fleet[3].get());
    ok &= check(spiker != nullptr && spiker->spikes() > 0, "spiking controller produced motor spikes");

    if (opt.promote) {
        std::cout << "\n[phase 2] promoting Rover_0 to a real hardware target over UDP\n";
        const std::string hostAddr = "127.0.0.1:" + std::to_string(kHostPort);
        const std::string robotAddr = "127.0.0.1:" + std::to_string(kRobotPort);
        auto hostStream = std::shared_ptr<comm::Stream>(comm::CreateUdpStream(kHostPort));
        RobotEmulator emulator(world, hostAddr);
        emulator.start();

        const auto handle = fleet[0]->handle();
        const auto before = *world.pose(handle);
        json::Value spawn = json::Value::object();
        spawn["cmd"] = "spawn";
        spawn["agent"] = fleet[0]->name();
        spawn["x"] = before.x;
        spawn["y"] = before.y;
        spawn["theta"] = before.theta;
        (void)hostStream->transmit(robotAddr, spawn.dump());
        std::this_thread::sleep_for(kRobotStartupDelay);

        world.attachLink(std::make_shared<sim::StreamRobotLink>(hostStream, robotAddr));
        ok &= check(world.setRunTarget(handle, sim::RunTarget::RealHardware), "run target switched to RealHardware");

        const int realSteps = static_cast<int>(opt.realSeconds / kStepSeconds);
        for (int i = 0; i < realSteps; ++i) {
            for (auto& r : fleet) r->step(kStepSeconds * kMillisecondsPerSecond);
            world.step(kStepSeconds);
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(kStepSeconds * kMillisecondsPerSecond)));
        }
        emulator.stop();
        const auto after = *world.pose(handle);
        const double moved = std::hypot(after.x - before.x, after.y - before.y);
        const auto* state = world.agent(handle);
        std::cout << "  Rover_0 moved " << moved
                  << " m under hardware control; telemetry frames=" << (state->remote ? state->remote->sequence : 0)
                  << "; commands delivered=" << emulator.commandsReceived() << "\n";
        ok &= check(state->remote && state->remote->sequence > 20, "telemetry streamed back from the hardware target");
        ok &= check(emulator.commandsReceived() > 20, "drive commands reached the hardware target");
        ok &= check(moved > 0.5, "the same Rover node moved the real target with unchanged control code");
    }

    {
        std::ofstream usd(opt.usdPath);
        usd << world.toUsda();
    }
    std::ifstream check_usd(opt.usdPath);
    const std::string usdText((std::istreambuf_iterator<char>(check_usd)), std::istreambuf_iterator<char>());
    ok &= check(usdText.starts_with("#usda 1.0") && usdText.find("Rover_0") != std::string::npos &&
                    usdText.find("timeSamples") != std::string::npos,
                "USD stage with recorded trajectories written to " + opt.usdPath);

    for (auto& r : fleet) r->shutdown();
    coordinator.shutdown();
    std::cout << "\n" << (ok ? "ARRIVAL OK" : "ARRIVAL FAILED") << "\n";
    return ok ? 0 : 1;
}
