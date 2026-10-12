#include "vr.hpp"

#include <cmath>
#include <format>

#include <gygax/core/json.hpp>

#include <glyder/draw.hpp>
#include <glyder/kit.hpp>

namespace wargames {

namespace wire = glyder::wire;
namespace kit = glyder::kit;

namespace {

kit::Shape shapeOf(Kind k) {
    switch (k) {
    case Kind::Bunker: return kit::Shape::Bunker;
    case Kind::Turret: return kit::Shape::Turret;
    case Kind::Tank: return kit::Shape::Tank;
    case Kind::Interceptor: return kit::Shape::Interceptor;
    case Kind::Supply: return kit::Shape::Quadcopter;
    case Kind::Swarmer: return kit::Shape::Swarmer;
    case Kind::Bruiser: return kit::Shape::Bruiser;
    case Kind::Spitter: return kit::Shape::Spitter;
    }
    return kit::Shape::Marker;
}

float heightOf(Kind k) {
    return k == Kind::Interceptor ? 60.0f : k == Kind::Supply ? 35.0f : 0.0f;
}

glyder::Handling handlingOf(Kind k) {
    const Spec& s = specOf(k);
    const auto speed = static_cast<float>(s.speed);
    if (k == Kind::Tank) return {glyder::Handling::Model::Ground, 0, speed, 0.6f, 0, 3};
    return {glyder::Handling::Model::Hover, 0, speed, 1.4f, 4, 10};
}

glyder::HostOptions options(std::string code, bool open, std::uint16_t port) {
    glyder::HostOptions o;
    o.name = "wargames";
    o.title = "WARGAMES";
    o.subtitle = "the last stand";
    o.secret = std::move(code);
    o.open = open;
    o.port = port ? port : wire::kDefaultPort;
    o.environment = static_cast<std::uint8_t>(glyder::render::Environment::Dusk);
    o.radius = 1300;
    o.snapshotHz = 30;
    o.help = "Point at a tank, interceptor or supply drone and pull the trigger to take it from its agent.\n"
             "order:rally auto\norder:rally N\norder:rally E\norder:rally S\norder:rally W\norder:weapons free\norder:weapons tight\n"
             "order:drones hunt\norder:drones recall\norder:produce tank\norder:produce interceptor";
    return o;
}

} // namespace

WarVr::WarVr(Battlefield& field, std::string code, bool open, std::uint16_t port)
    : field_(field), host_(options(std::move(code), open, port)) {}

bool WarVr::start(std::string* error) {
    return host_.start(error);
}

void WarVr::stop() {
    host_.stop();
}

std::string WarVr::banner() const {
    std::string addrs;
    for (const auto& a : glyder::net::localAddresses()) addrs += "  " + a.str().substr(0, a.str().rfind(':'));
    return std::format("WARGAMES in VR: open glyder on the headset and choose WARGAMES{}.\n  this computer:{}  (UDP {})\n",
                       host_.pairingCode().empty() ? "" : std::format(" (pairing code {})", host_.pairingCode()),
                       addrs.empty() ? "  no network" : addrs, host_.port());
}

void WarVr::event(const std::string& line) {
    host_.event(wire::EventKind::Info, line, field_.time());
}

void WarVr::tick(double realSeconds) {
    clock_ += realSeconds;
    host_.service(*this, std::min(realSeconds, 0.1));
    publish();
}

void WarVr::publish() {
    std::vector<wire::EntityState> out;
    std::vector<wire::Effect> fx;
    const double t = field_.time();
    for (const auto& e : field_.entities()) {
        if (!e.alive) continue;
        const Spec& s = specOf(e.kind);
        wire::EntityState st;
        st.id = e.id;
        st.shape = static_cast<std::uint16_t>(shapeOf(e.kind));
        st.team = s.human ? 1 : 4;
        st.tint = kit::palette::teams[st.team];
        st.pos = {static_cast<float>(e.pos.x), static_cast<float>(e.pos.y), heightOf(e.kind)};
        // Velocity from the last two ticks, so headsets glide units between them.
        auto& [lastT, lastP] = last_[e.id];
        if (t > lastT + 1e-6) {
            velocity_[e.id] = {static_cast<float>((e.pos.x - lastP.x) / (t - lastT)), static_cast<float>((e.pos.y - lastP.y) / (t - lastT)),
                               0};
            lastT = t;
            lastP = e.pos;
        }
        if (auto m = motion_.find(e.id); m != motion_.end()) {
            st.pos = m->second.pos;
            st.vel = m->second.velocity();
            st.q = m->second.attitude(handlingOf(e.kind).model);
        } else {
            st.vel = velocity_[e.id];
            const float speed = std::hypot(st.vel.x, st.vel.y);
            st.q = speed > 0.3f ? glyder::alongVelocity(st.vel)
                                : glyder::attitude(std::atan2(static_cast<float>(-e.pos.x), static_cast<float>(-e.pos.y)));
            if (!s.human && speed <= 0.3f) st.q = glyder::attitude(std::atan2(static_cast<float>(-e.pos.x), static_cast<float>(-e.pos.y)));
        }
        st.flags = wire::kAlive;
        if (s.human && s.speed > 0) st.flags |= wire::kDrivable;
        if (e.engaged) st.flags |= wire::kFiring;
        out.push_back(st);
        host_.info(
            e.id, e.name,
            std::format("{}, {:.0f}% hp{}", roleOf(e.kind), 100.0 * e.hp / s.hp, s.armed ? std::format(", ammo {:.0f}", e.ammo) : ""));
        if (e.engaged)
            if (const Entity* target = field_.find(e.engaged); target && target->alive) {
                wire::Effect shot;
                shot.kind = wire::EffectKind::Tracer;
                shot.colour = s.human ? glyder::hex(0x9FF6FF) : kit::palette::acid;
                shot.a = st.pos + glyder::Vec3{0, 0, s.human ? 1.8f : 1.2f};
                shot.b = {static_cast<float>(target->pos.x), static_cast<float>(target->pos.y), heightOf(target->kind) + 1.0f};
                shot.width = s.human ? 0.25f : 0.5f;
                shot.intensity = static_cast<std::uint8_t>(140 + 115 * (0.5 + 0.5 * std::sin(clock_ * 25.0 + e.id)));
                fx.push_back(shot);
            }
    }
    host_.publish(t, std::move(out), std::move(fx));
    const auto& st = field_.stats();
    using wire::Tone;
    host_.status({{"T+", std::format("{:.0f} S", t), Tone::Normal},
                  {"BUNKER", std::format("{:.0f}", field_.bunkerHp()), field_.bunkerHp() < 1500 ? Tone::Alert : Tone::Good},
                  {"WAVES", std::to_string(field_.outcome().wavesSpawned), Tone::Normal},
                  {"ALIENS", std::format("{} KILLED / {}", st.aliensKilled, st.aliensSpawned), Tone::Good},
                  {"DEFENDERS LOST", std::to_string(st.humansLost), st.humansLost ? Tone::Warn : Tone::Good},
                  {"MATERIEL", std::format("{:.0f}", field_.materiel()), Tone::Normal}});
}

std::optional<std::string> WarVr::refuse(std::uint32_t id) {
    const Entity* e = field_.find(id);
    if (!e || !e->alive) return "that unit is gone";
    const Spec& s = specOf(e->kind);
    if (!s.human) return "the hive is not yours to fly";
    if (s.speed <= 0) return "a fixed emplacement: it cannot move";
    return std::nullopt;
}

std::optional<glyder::Pilot::Wait> WarVr::busy(std::uint32_t id) {
    const Entity* e = field_.find(id);
    if (e && e->engaged) return Wait{"finishing an engagement", 1.0f};
    return std::nullopt;
}

glyder::Handling WarVr::take(std::uint32_t id) {
    const Entity* e = field_.find(id);
    glyder::Motion m;
    if (e) {
        m.pos = {static_cast<float>(e->pos.x), static_cast<float>(e->pos.y), heightOf(e->kind)};
        const auto v = velocity_[id];
        m.heading = std::hypot(v.x, v.y) > 0.3f ? std::atan2(v.x, v.y) : std::atan2(-m.pos.x, -m.pos.y);
    }
    motion_[id] = m;
    event(std::format("a pilot has taken {} from its agent", e ? e->name : "a unit"));
    return e ? handlingOf(e->kind) : glyder::Handling{};
}

void WarVr::drive(std::uint32_t id, const wire::Control& c, bool fresh, double dt) {
    const Entity* e = field_.find(id);
    auto it = motion_.find(id);
    if (!e || it == motion_.end()) return;
    const glyder::Handling h = handlingOf(e->kind);
    if (fresh) {
        const float axes[6] = {c.axes[0], c.axes[1], c.axes[2], c.axes[3], c.axes[4], c.axes[5]};
        glyder::step(it->second, h, axes, static_cast<float>(dt), heightOf(e->kind) * 0.5f);
    } else {
        glyder::hold(it->second, h, static_cast<float>(dt), 0);
    }
    it->second.pos.z = heightOf(e->kind);
    // The battlefield moves units on its own tick: point it where the pilot is headed.
    const glyder::Vec3 lead = it->second.pos + it->second.velocity() * 1.0f;
    gygax::json::Value orders = gygax::json::Value::array();
    gygax::json::Value move = gygax::json::Value::object();
    gygax::json::Value xy = gygax::json::Value::array();
    xy.push(static_cast<double>(lead.x));
    xy.push(static_cast<double>(lead.y));
    move["move"] = std::move(xy);
    orders.push(std::move(move));
    try {
        field_.applyOrders(id, orders);
    } catch (const std::exception&) {
    }
    // Keep the pilot's picture and the battlefield's in step.
    const glyder::Vec3 truth{static_cast<float>(e->pos.x), static_cast<float>(e->pos.y), heightOf(e->kind)};
    it->second.pos += (truth - it->second.pos) * 0.05f;
}

void WarVr::give(std::uint32_t id) {
    motion_.erase(id);
    event("the pilot has handed back; its agent takes over");
}

bool WarVr::exists(std::uint32_t id) {
    const Entity* e = field_.find(id);
    return e && e->alive;
}

} // namespace wargames
