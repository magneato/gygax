#include <glyder/demo.hpp>

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
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

// What every built-in scene is: a world of nodes a pilot can drop into.
class Scene : public Pilot {
public:
    virtual void step(double dt) = 0;
    virtual void publish(Host& host) = 0;
    [[nodiscard]] virtual HostOptions world() const = 0;

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

protected:
    void add(std::uint32_t id, kit::Shape s, std::uint8_t team, Handling h, std::string name, Vec3 pos) {
        Node n{id, s, team, h, {}, std::move(name)};
        n.motion.pos = pos;
        nodes_.emplace(id, std::move(n));
    }
    void step1(Node& n, const float (&axes)[6], double dt) {
        glyder::step(n.motion, n.handling, axes, static_cast<float>(dt), 0);
        // Keep everything in the arena.
        const float r = std::sqrt(n.motion.pos.x * n.motion.pos.x + n.motion.pos.y * n.motion.pos.y);
        if (r > arena_) n.motion.pos = n.motion.pos * (arena_ / r);
        if (n.handling.model == Handling::Model::Hover) n.motion.pos.z = std::min(n.motion.pos.z, 40.0f);
    }
    void publishNodes(std::vector<wire::EntityState>& out) const {
        for (const auto& [id, n] : nodes_) {
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
    }
    void nodeInfo(Host& host) const {
        for (const auto& [id, n] : nodes_) {
            const float kmh = std::fabs(n.motion.speed) * 3.6f;
            host.info(id, n.name, n.manual ? "piloted" : std::to_string(static_cast<int>(kmh)) + " km/h, routine");
        }
    }
    [[nodiscard]] std::string clock() const {
        char text[16];
        std::snprintf(text, sizeof text, "%02d:%02d", static_cast<int>(std::fmod(t_ / 60.0, 60.0)), static_cast<int>(std::fmod(t_, 60.0)));
        return text;
    }

    std::map<std::uint32_t, Node> nodes_;
    double t_{0};
    float arena_{140};
};

// Calm: the Construct as it always was, minus anything that flashes. Slow, predictable routines.
class Calm final : public Scene {
public:
    Calm() {
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

    void step(double dt) override {
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

    void publish(Host& host) override {
        std::vector<wire::EntityState> out;
        publishNodes(out);
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
        // One soft ring, breathing slowly outward (no flashes, nothing sudden).
        std::vector<wire::Effect> fx;
        const double phase = std::fmod(t_, 8.0) / 8.0;
        wire::Effect ring;
        ring.kind = wire::EffectKind::Ring;
        ring.colour = kit::palette::teams[1];
        ring.a = {0, 0, 0.05f};
        ring.b = {6.0f + 4.0f * static_cast<float>(phase), 0, 0};
        ring.width = 0.06f;
        ring.intensity = static_cast<std::uint8_t>(120 * std::sin(phase * kPi));
        fx.push_back(ring);
        host.publish(t_, std::move(out), std::move(fx));
        nodeInfo(host);
        host.status({{"SCENE", "CALM", wire::Tone::Good}, {"TIME", clock(), wire::Tone::Normal},
                     {"NODES", std::to_string(nodes_.size()), wire::Tone::Good}});
    }

    [[nodiscard]] HostOptions world() const override {
        HostOptions o;
        o.title = "THE CONSTRUCT: CALM";
        o.subtitle = "slow, predictable routines";
        o.environment = static_cast<std::uint8_t>(render::Environment::Construct);
        o.radius = 80;
        o.help = "Point at a node and pull the trigger to drop in. Everything here moves slowly and nothing flashes.";
        return o;
    }
};

// Chaos: everything at once. A hive swarm spirals in wave after wave, missiles arc across the
// arena and burst, turrets hose tracers, and fast drones weave through it. Drop into any of them.
class Chaos final : public Scene {
public:
    Chaos() {
        arena_ = 150;
        add(1, kit::Shape::Interceptor, 1, {Handling::Model::Air, 20, 55, 1.6f, 9, 10}, "INTERCEPTOR 1", {0, 30, 30});
        add(2, kit::Shape::Interceptor, 1, {Handling::Model::Air, 20, 55, 1.6f, 9, 10}, "INTERCEPTOR 2", {0, -30, 45});
        add(3, kit::Shape::Quadcopter, 1, {Handling::Model::Hover, 0, 14, 2.4f, 5, 10}, "QUAD A", {10, 0, 12});
        add(4, kit::Shape::Quadcopter, 1, {Handling::Model::Hover, 0, 14, 2.4f, 5, 10}, "QUAD B", {-10, 0, 18});
        add(5, kit::Shape::Helicopter, 3, {Handling::Model::Hover, 0, 30, 0.9f, 6, 5}, "HELO", {40, 40, 35});
        add(6, kit::Shape::Rover, 1, {Handling::Model::Ground, 0, 4, 1.2f, 0, 3}, "ROVER", {0, -6, 0});
        for (auto& [id, n] : nodes_) {
            n.phase = static_cast<double>(id) * 1.7;
            if (n.handling.model == Handling::Model::Air) n.motion.speed = 40;
        }
    }

    void step(double dt) override {
        t_ += dt;
        for (auto& [id, n] : nodes_) {
            if (n.manual) continue;
            float axes[6] = {0, 0, 0, 0, 0, 0};
            const double p = t_ + n.phase;
            switch (n.handling.model) {
            case Handling::Model::Air: // hard jinking turns, climbing and diving
                axes[3] = static_cast<float>(std::sin(p * 0.9) * std::cos(p * 0.37));
                axes[4] = 0.5f * static_cast<float>(std::sin(p * 0.6));
                axes[5] = 0.6f;
                if (n.motion.pos.z < 15) axes[4] = 0.8f;
                if (n.motion.pos.z > 90) axes[4] = -0.8f;
                break;
            case Handling::Model::Hover: // darting between points
                axes[0] = static_cast<float>(std::sin(p * 1.3));
                axes[1] = static_cast<float>(std::cos(p * 1.1));
                axes[2] = 0.5f * static_cast<float>(std::sin(p * 0.7));
                axes[3] = 0.6f * static_cast<float>(std::sin(p * 0.5));
                if (n.motion.pos.z < 6) axes[2] = 0.8f;
                break;
            default: // dodging about the middle
                axes[0] = 0.9f;
                axes[3] = static_cast<float>(std::sin(p * 0.8));
                break;
            }
            step1(n, axes, dt);
        }
        // Launch missiles on a steady drumbeat.
        while (nextLaunch_ <= t_) {
            Missile m;
            const double a = unit() * 2 * kPi;
            m.from = {static_cast<float>(140 * std::cos(a)), static_cast<float>(140 * std::sin(a)), 2};
            m.to = {static_cast<float>((unit() - 0.5) * 80), static_cast<float>((unit() - 0.5) * 80), 0};
            m.born = nextLaunch_;
            m.flight = 3.0 + unit() * 2.0;
            m.id = nextMissile_++;
            if (nextMissile_ > 2999) nextMissile_ = 2000;
            missiles_.push_back(m);
            nextLaunch_ += 0.35 + unit() * 0.5;
        }
        std::erase_if(missiles_, [&](const Missile& m) { return t_ > m.born + m.flight + kBurst; });
    }

    void publish(Host& host) override {
        std::vector<wire::EntityState> out;
        publishNodes(out);
        std::vector<wire::Effect> fx;
        // The swarm: each swarmer spirals in from the edge over its wave, then starts again.
        const int wave = static_cast<int>(t_ / kWave) + 1;
        std::vector<Vec3> swarm;
        for (int i = 0; i < kSwarm; ++i) {
            const double life = std::fmod(t_ + i * (kWave / kSwarm), kWave) / kWave; // 0 at the edge, 1 at the centre
            const double a = i * 2.39996 + life * 5.0;
            const double r = 150 * (1 - life) + 4;
            const double z = 6 + 30 * (1 - life) + 4 * std::sin(t_ * 3 + i);
            const Vec3 pos{static_cast<float>(r * std::cos(a)), static_cast<float>(r * std::sin(a)), static_cast<float>(z)};
            const Vec3 vel{static_cast<float>(-150 / kWave * std::cos(a) - r * 5 / kWave * std::sin(a)),
                           static_cast<float>(-150 / kWave * std::sin(a) + r * 5 / kWave * std::cos(a)), static_cast<float>(-30 / kWave)};
            wire::EntityState e;
            e.id = 1000 + static_cast<std::uint32_t>(i);
            e.shape = static_cast<std::uint16_t>(kit::Shape::Swarmer);
            e.team = 2;
            e.pos = pos;
            e.vel = vel;
            e.q = alongVelocity(vel);
            e.tint = kit::palette::teams[2];
            out.push_back(e);
            swarm.push_back(pos);
        }
        // Missiles in flight, and a burst where each one lands.
        int inbound = 0;
        for (const auto& m : missiles_) {
            const double u = (t_ - m.born) / m.flight;
            if (u < 1) {
                ++inbound;
                const float uf = static_cast<float>(u);
                const float arc = 60.0f * 4 * uf * (1 - uf);
                const Vec3 pos = m.from + (m.to - m.from) * uf + Vec3{0, 0, arc};
                const Vec3 vel = (m.to - m.from) * static_cast<float>(1 / m.flight) + Vec3{0, 0, static_cast<float>(240 * (1 - 2 * u) / m.flight)};
                wire::EntityState e;
                e.id = m.id;
                e.shape = static_cast<std::uint16_t>(kit::Shape::Missile);
                e.team = 2;
                e.flags = wire::kAlive | wire::kFocus;
                e.pos = pos;
                e.vel = vel;
                e.q = alongVelocity(vel);
                e.tint = kit::palette::teams[2];
                out.push_back(e);
                wire::Effect trail;
                trail.kind = wire::EffectKind::Trail;
                trail.colour = kit::palette::engine;
                trail.a = pos;
                trail.b = pos - vel * 0.25f;
                trail.width = 0.4f;
                fx.push_back(trail);
            } else {
                const float k = static_cast<float>((t_ - m.born - m.flight) / kBurst);
                wire::Effect ring;
                ring.kind = wire::EffectKind::Ring;
                ring.colour = kit::palette::engine;
                ring.a = m.to + Vec3{0, 0, 0.1f};
                ring.b = {2 + 14 * k, 0, 0};
                ring.width = 0.5f;
                ring.intensity = static_cast<std::uint8_t>(255 * (1 - k));
                fx.push_back(ring);
            }
        }
        // Four turrets hose tracers at the nearest swarmer.
        std::uint32_t sid = 3000;
        for (int i = 0; i < 4; ++i) {
            const float a = static_cast<float>(i) * kPi * 0.5f + kPi * 0.25f;
            const Vec3 at{28 * std::cos(a), 28 * std::sin(a), 0};
            const Vec3* best = nullptr;
            float bestD = 1e9f;
            for (const auto& p : swarm) {
                const Vec3 d = p - at;
                const float dd = dot(d, d);
                if (dd < bestD) {
                    bestD = dd;
                    best = &p;
                }
            }
            wire::EntityState t;
            t.id = sid++;
            t.shape = static_cast<std::uint16_t>(kit::Shape::Turret);
            t.team = 1;
            t.pos = at;
            t.tint = kit::palette::teams[1];
            if (best) t.q = attitude(std::atan2(best->x - at.x, best->y - at.y));
            const bool firing = best && bestD < 70.0f * 70.0f;
            t.flags = static_cast<std::uint8_t>(wire::kAlive | (firing ? wire::kFiring : 0));
            out.push_back(t);
            if (firing) {
                wire::Effect tracer;
                tracer.kind = wire::EffectKind::Tracer;
                tracer.colour = kit::palette::lamp;
                tracer.a = at + Vec3{0, 0, 2};
                tracer.b = *best;
                tracer.width = 0.15f;
                fx.push_back(tracer);
            }
        }
        host.publish(t_, std::move(out), std::move(fx));
        nodeInfo(host);
        if (wave != lastWave_) {
            lastWave_ = wave;
            host.event(wire::EventKind::Opfor, "Wave " + std::to_string(wave) + ": the swarm is inbound", t_);
        }
        host.status({{"SCENE", "CHAOS", wire::Tone::Alert},
                     {"WAVE", std::to_string(wave), wire::Tone::Warn},
                     {"MISSILES", std::to_string(inbound), inbound ? wire::Tone::Alert : wire::Tone::Normal},
                     {"SWARM", std::to_string(kSwarm), wire::Tone::Warn},
                     {"TIME", clock(), wire::Tone::Normal}});
    }

    [[nodiscard]] HostOptions world() const override {
        HostOptions o;
        o.title = "THE CONSTRUCT: CHAOS";
        o.subtitle = "everything at once";
        o.environment = static_cast<std::uint8_t>(render::Environment::Night);
        o.radius = 160;
        o.help = "Point at a drone and pull the trigger to drop in. The swarm, the missiles and the turrets never stop.";
        return o;
    }

private:
    struct Missile {
        std::uint32_t id{0};
        Vec3 from, to;
        double born{0}, flight{4};
    };
    static constexpr int kSwarm = 36;
    static constexpr double kWave = 14.0;
    static constexpr double kBurst = 0.8;
    double unit() { // deterministic, so a capture is repeatable
        seed_ = seed_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(seed_ >> 11) / static_cast<double>(1ULL << 53);
    }
    std::vector<Missile> missiles_;
    std::uint64_t seed_{1984};
    double nextLaunch_{0.5};
    std::uint32_t nextMissile_{2000};
    int lastWave_{0};
};

std::unique_ptr<Scene> makeScene(DemoScene which) {
    if (which == DemoScene::Chaos) return std::make_unique<Chaos>();
    return std::make_unique<Calm>();
}

} // namespace

struct DemoWorld::Impl {
    std::unique_ptr<Host> host;
    std::unique_ptr<Scene> world;
    std::atomic<DemoScene> current{DemoScene::Calm}, wanted{DemoScene::Calm};
    std::jthread thread;
    std::atomic<bool> running{false};
};

DemoWorld::DemoWorld() : impl_(std::make_unique<Impl>()) {}
DemoWorld::~DemoWorld() {
    stop();
}

bool DemoWorld::start(std::string* error, DemoScene scene) {
    impl_->world = makeScene(scene);
    impl_->current = impl_->wanted = scene;
    HostOptions o = impl_->world->world();
    o.name = "construct";
    o.bind = "127.0.0.1";
    o.port = 0;
    o.snapshotHz = 60;
    impl_->host = std::make_unique<Host>(o);
    if (!impl_->host->start(error)) return false;
    impl_->running = true;
    impl_->thread = std::jthread([this] {
        using clock = std::chrono::steady_clock;
        auto next = clock::now();
        const double dt = 1.0 / 120.0;
        while (impl_->running) {
            if (const DemoScene want = impl_->wanted.load(); want != impl_->current.load()) {
                auto fresh = makeScene(want);
                impl_->host->reconfigure(fresh->world(), nullptr, impl_->world.get());
                impl_->world = std::move(fresh);
                impl_->current = want;
            }
            impl_->world->step(dt);
            impl_->host->service(*impl_->world, dt);
            impl_->world->publish(*impl_->host);
            next += std::chrono::microseconds(8333);
            std::this_thread::sleep_until(next);
        }
    });
    return true;
}

void DemoWorld::setScene(DemoScene scene) {
    impl_->wanted = scene;
}

DemoScene DemoWorld::scene() const {
    return impl_->wanted.load();
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
