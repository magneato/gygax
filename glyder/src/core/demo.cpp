#include <glyder/demo.hpp>

#include <atomic>
#include <cstdio>
#include <map>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include <glyder/draw.hpp>
#include <glyder/handling.hpp>
#include <glyder/kit.hpp>

namespace glyder {

namespace {

struct Node {
    std::uint32_t id;
    kit::Shape shape;
    std::uint8_t team;
    Handling handling;
    Motion motion;
    std::string name;
    bool manual{false};
    double phase{0};
};

class Construct final : public Pilot {
public:
    Construct() {
        add(1, kit::Shape::Rover, 1, {Handling::Model::Ground, 0, 2.5f, 0.9f, 0, 2}, "ROVER", {0, 8, 0});
        add(2, kit::Shape::Quadcopter, 1, {Handling::Model::Hover, 0, 8, 1.6f, 3, 6}, "QUAD", {4, 14, 3});
        add(3, kit::Shape::Kite, 1, {Handling::Model::Air, 10, 28, 0.9f, 4, 4}, "KITE R1", {0, 40, 25});
        add(4, kit::Shape::Truck, 3, {Handling::Model::Ground, 0, 12, 0.5f, 0, 3}, "TRUCK", {-20, 25, 0});
        add(5, kit::Shape::FixedWing, 1, {Handling::Model::Air, 14, 32, 0.7f, 5, 3}, "SURVEY", {30, 60, 60});
        nodes_[2].motion.heading = 0.5f;
        nodes_[3].motion.speed = 20;
        nodes_[4].motion.speed = 6;
        nodes_[5].motion.speed = 22;
    }

    void step(double dt) {
        t_ += dt;
        for (auto& [id, n] : nodes_) {
            if (n.manual) continue;
            float axes[6] = {0, 0, 0, 0, 0, 0};
            switch (n.shape) {
            case kit::Shape::Rover: // a slow figure of eight
                axes[0] = 0.6f;
                axes[3] = 0.8f * static_cast<float>(std::sin(t_ * 0.25));
                break;
            case kit::Shape::Quadcopter: { // hop between waypoints
                const double seg = std::fmod(t_, 16.0);
                axes[0] = seg < 6 ? 0.5f : 0.0f;
                axes[3] = seg >= 6 && seg < 8 ? 0.9f : 0.0f;
                axes[2] = seg >= 8 && seg < 10 ? 0.6f : (seg >= 12 && seg < 14 ? -0.6f : 0.0f);
                break;
            }
            case kit::Shape::Kite: // racetrack with straights
                axes[3] = std::fmod(t_, 20.0) > 12 ? 0.85f : 0.0f;
                axes[4] = 0.08f * static_cast<float>(std::sin(t_ * 0.3));
                break;
            case kit::Shape::Truck:
                axes[0] = 0.5f;
                axes[3] = std::fmod(t_, 18.0) > 13 ? 0.9f : 0.0f;
                break;
            default: // wide survey orbit
                axes[3] = 0.35f;
                break;
            }
            step1(n, axes, dt);
        }
    }

    void publish(Host& host) {
        std::vector<wire::EntityState> out;
        for (auto& [id, n] : nodes_) {
            wire::EntityState e;
            e.id = id;
            e.shape = static_cast<std::uint16_t>(n.shape);
            e.team = n.team;
            e.flags = wire::kAlive | wire::kDrivable;
            e.pos = n.motion.pos;
            e.vel = n.motion.velocity();
            e.q = n.motion.attitude(n.handling.model);
            e.tint = kit::palette::teams[n.team];
            out.push_back(e);
        }
        // Scenery: markers on a ring and a few monoliths.
        std::uint32_t sid = 100;
        for (int i = 0; i < 12; ++i) {
            const float a = static_cast<float>(i) * kPi / 6;
            wire::EntityState m;
            m.id = sid++;
            m.shape = static_cast<std::uint16_t>(kit::Shape::Marker);
            m.team = 1;
            m.pos = {60 * std::cos(a), 60 * std::sin(a), 0};
            m.q = attitude(a);
            m.tint = kit::palette::teams[1];
            out.push_back(m);
        }
        for (int i = 0; i < 3; ++i) {
            wire::EntityState m;
            m.id = sid++;
            m.shape = static_cast<std::uint16_t>(kit::Shape::Node);
            m.team = 5;
            m.pos = {-12.0f + 12.0f * static_cast<float>(i), -18, 0};
            m.q = attitude(kPi * 0.5f);
            m.tint = kit::palette::teams[5];
            m.scale = 1.5f;
            out.push_back(m);
        }
        std::vector<wire::Effect> fx;
        // The kite paints the ground with a soft marker when it is on its straight.
        const Node& kite = nodes_.at(3);
        wire::Effect beam;
        beam.kind = wire::EffectKind::Laser;
        beam.colour = kit::palette::laserG;
        beam.a = kite.motion.pos + Vec3{0, 0, -0.1f};
        beam.b = {kite.motion.pos.x, kite.motion.pos.y + 6, 0.05f};
        beam.width = 0.05f;
        if (!kite.manual && std::fmod(t_, 20.0) < 12) fx.push_back(beam);
        wire::Effect ring;
        ring.kind = wire::EffectKind::Ring;
        ring.colour = kit::palette::teams[1];
        ring.a = {0, 0, 0.05f};
        ring.b = {6.0f + 2.0f * static_cast<float>(std::fmod(t_, 3.0)), 0, 0};
        ring.width = 0.06f;
        ring.intensity = static_cast<std::uint8_t>(255 * (1.0 - std::fmod(t_, 3.0) / 3.0));
        fx.push_back(ring);
        host.publish(t_, std::move(out), std::move(fx));
        for (auto& [id, n] : nodes_) {
            const float kmh = std::fabs(n.motion.speed) * 3.6f;
            host.info(id, n.name, n.manual ? "piloted" : std::to_string(static_cast<int>(kmh)) + " km/h, routine");
        }
        const double mm = std::fmod(t_ / 60.0, 60.0), ss = std::fmod(t_, 60.0);
        char clock[16];
        std::snprintf(clock, sizeof clock, "%02d:%02d", static_cast<int>(mm), static_cast<int>(ss));
        host.status({{"WORLD", "THE CONSTRUCT", wire::Tone::Normal},
                     {"TIME", clock, wire::Tone::Normal},
                     {"NODES", std::to_string(nodes_.size()), wire::Tone::Good}});
    }

    // Pilot
    std::optional<std::string> refuse(std::uint32_t id) override {
        return nodes_.count(id) ? std::nullopt : std::optional<std::string>("scenery cannot be driven");
    }
    std::optional<Wait> busy(std::uint32_t id) override {
        const Node& n = nodes_.at(id);
        if (std::fabs(n.motion.yawRate) > 0.15f) return Wait{"finishing a turn", 1.5f};
        if (std::fabs(n.motion.climb) > 0.5f) return Wait{n.motion.climb > 0 ? "finishing a climb" : "finishing a descent", 1.0f};
        return std::nullopt;
    }
    Handling take(std::uint32_t id) override {
        Node& n = nodes_.at(id);
        n.manual = true;
        return n.handling;
    }
    void drive(std::uint32_t id, const wire::Control& c, bool fresh, double dt) override {
        Node& n = nodes_.at(id);
        if (!fresh) {
            hold(n.motion, n.handling, static_cast<float>(dt), 0);
            return;
        }
        const float axes[6] = {c.axes[0], c.axes[1], c.axes[2], c.axes[3], c.axes[4], c.axes[5]};
        step1(n, axes, dt);
    }
    void give(std::uint32_t id) override { nodes_.at(id).manual = false; }
    bool exists(std::uint32_t id) override { return nodes_.count(id) > 0; }

private:
    void add(std::uint32_t id, kit::Shape s, std::uint8_t team, Handling h, std::string name, Vec3 pos) {
        Node n{id, s, team, h, {}, std::move(name)};
        n.motion.pos = pos;
        nodes_.emplace(id, std::move(n));
    }
    void step1(Node& n, const float (&axes)[6], double dt) {
        glyder::step(n.motion, n.handling, axes, static_cast<float>(dt), 0);
        // Keep everything in the arena.
        const float r = std::sqrt(n.motion.pos.x * n.motion.pos.x + n.motion.pos.y * n.motion.pos.y);
        if (r > 140) n.motion.pos = n.motion.pos * (140 / r);
        if (n.handling.model == Handling::Model::Hover) n.motion.pos.z = std::min(n.motion.pos.z, 40.0f);
    }
    std::map<std::uint32_t, Node> nodes_;
    double t_{0};
};

} // namespace

struct DemoWorld::Impl {
    std::unique_ptr<Host> host;
    Construct world;
    std::jthread thread;
    std::atomic<bool> running{false};
};

DemoWorld::DemoWorld() : impl_(std::make_unique<Impl>()) {}
DemoWorld::~DemoWorld() {
    stop();
}

bool DemoWorld::start(std::string* error) {
    HostOptions o;
    o.name = "construct";
    o.title = "THE CONSTRUCT";
    o.subtitle = "glyder's training world";
    o.bind = "127.0.0.1";
    o.port = 0;
    o.snapshotHz = 60;
    o.environment = static_cast<std::uint8_t>(render::Environment::Construct);
    o.radius = 80;
    o.help = "Point at a node and pull the trigger to drop in.";
    impl_->host = std::make_unique<Host>(o);
    if (!impl_->host->start(error)) return false;
    impl_->running = true;
    impl_->thread = std::jthread([this] {
        using clock = std::chrono::steady_clock;
        auto next = clock::now();
        const double dt = 1.0 / 120.0;
        while (impl_->running) {
            impl_->world.step(dt);
            impl_->host->service(impl_->world, dt);
            impl_->world.publish(*impl_->host);
            next += std::chrono::microseconds(8333);
            std::this_thread::sleep_until(next);
        }
    });
    return true;
}

void DemoWorld::stop() {
    if (!impl_ || !impl_->running) return;
    impl_->running = false;
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->host->stop();
}

std::uint16_t DemoWorld::port() const {
    return impl_->host ? impl_->host->port() : 0;
}

} // namespace glyder
