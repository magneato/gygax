#include "vr.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include <glyder/draw.hpp>
#include <glyder/kit.hpp>

namespace ohflock {

namespace wire = glyder::wire;
namespace kit = glyder::kit;
using glyder::Vec3;

namespace {

constexpr std::uint32_t kShotBase = 100000, kSatBase = 200000;

Vec3 v3(V3 p) {
    return {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
}

glyder::Rgb8 section(char s) {
    switch (s) {
    case 'R': return kit::palette::laserR;
    case 'G': return kit::palette::laserG;
    case 'B': return kit::palette::laserB;
    default: return kit::palette::bone;
    }
}

kit::Shape shapeOf(Kind k) {
    switch (k) {
    case Kind::Kite: return kit::Shape::Kite;
    case Kind::Fighter: return kit::Shape::Fighter;
    case Kind::Striker: return kit::Shape::Striker;
    case Kind::Sam: return kit::Shape::SamLauncher;
    case Kind::Aaa: return kit::Shape::AaaGun;
    case Kind::AcousticArray: return kit::Shape::AcousticArray;
    case Kind::Jammer: return kit::Shape::Jammer;
    case Kind::Truck: return kit::Shape::Truck;
    }
    return kit::Shape::Marker;
}

double hash(double x, double y) {
    const double s = std::sin(x * 127.1 + y * 311.7) * 43758.5453;
    return s - std::floor(s);
}

double noise(double x, double y) {
    const double xi = std::floor(x), yi = std::floor(y), xf = x - xi, yf = y - yi;
    const double u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
    const double a = hash(xi, yi), b = hash(xi + 1, yi), c = hash(xi, yi + 1), d = hash(xi + 1, yi + 1);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

// The navigation source in a word or two, for a headset's status panel.
std::string shortNav(const std::string& source) {
    if (source.find("Doppler (") != std::string::npos) return "LEO DOPPLER";
    if (source.find("awaiting") != std::string::npos) return "INERTIAL";
    if (source.find("inertial") != std::string::npos) return "INERTIAL";
    return source;
}

} // namespace

double ground(double x, double y) {
    // The replay's field at true scale (the replay exaggerates relief 2.4 times for legibility from
    // above; in a headset at 1:1 the real thing reads well).
    const double ridge = std::pow(std::abs(std::sin((x * 0.8 + y * 0.6) / 1300 + noise(x / 4000, y / 4000) * 2.4)), 2.2) * 95;
    const double swell = noise(x / 6000 + 9, y / 6000 + 3) * 70 + noise(x / 1500, y / 1500) * 12;
    const double road = std::min(1.0, std::abs(x) / 900);
    return ((ridge + swell) * (0.25 + 0.75 * road) - 30) * 0.4;
}

wire::Heightfield terrain() {
    wire::Heightfield f;
    f.w = f.h = 221;
    f.cell = 200;
    f.x0 = f.y0 = -22000;
    f.z.resize(static_cast<std::size_t>(f.w) * f.h);
    for (int j = 0; j < f.h; ++j)
        for (int i = 0; i < f.w; ++i)
            f.z[static_cast<std::size_t>(j) * f.w + static_cast<std::size_t>(i)] =
                static_cast<float>(ground(f.x0 + i * static_cast<double>(f.cell), f.y0 + j * static_cast<double>(f.cell)));
    return f;
}

glyder::HostOptions hostOptions(const std::string& code, bool open, std::uint16_t port) {
    glyder::HostOptions o;
    o.name = "ohflock";
    o.title = "OHFLOCK";
    o.subtitle = "the corridor, live";
    o.secret = code;
    o.open = open;
    o.port = port;
    o.environment = static_cast<std::uint8_t>(glyder::render::Environment::Night);
    o.originLat = Sim::kOriginLat;
    o.originLon = Sim::kOriginLon;
    o.groundZ = 0;
    o.radius = 9000;
    o.snapshotHz = 30;
    o.help = "Point at a kite and pull the trigger to drop in; it hands over between manoeuvres.\n"
             "order:posture screen\norder:posture tight\norder:posture wide\norder:lasers on\norder:lasers off\n"
             "order:acoustics on\norder:acoustics off\norder:corridor east\norder:corridor west\norder:corridor centre";
    return o;
}

std::size_t publish(glyder::Host& host, const Sim& sim, std::size_t fromEvent) {
    std::vector<wire::EntityState> out;
    std::vector<wire::Effect> fx;
    out.reserve(sim.units().size() + 64);

    std::map<int, bool> lasing;
    for (const auto& b : sim.beams()) lasing[b.from] = true;

    for (const auto& u : sim.units()) {
        if (!u.alive || u.gone) continue;
        wire::EntityState e;
        e.id = static_cast<std::uint32_t>(u.id);
        e.shape = static_cast<std::uint16_t>(shapeOf(u.kind));
        e.pos = v3(u.pos);
        e.vel = v3(u.vel);
        e.flags = wire::kAlive;
        switch (u.kind) {
        case Kind::Kite:
            e.team = 1;
            e.tint = section(u.section);
            e.flags |= wire::kDrivable;
            if (lasing.count(u.id)) e.flags |= wire::kFiring;
            e.q = glyder::alongVelocity(e.vel);
            break;
        case Kind::Fighter:
        case Kind::Striker:
            e.team = 2;
            e.tint = kit::palette::teams[2];
            e.q = glyder::alongVelocity(e.vel);
            break;
        case Kind::Truck:
            e.team = 3;
            e.tint = kit::palette::teams[3];
            e.pos.z = static_cast<float>(ground(u.pos.x, u.pos.y));
            e.q = glyder::attitude(0); // north up the road
            break;
        default: {
            e.team = 2;
            e.tint = kit::palette::teams[2];
            e.pos.z = static_cast<float>(ground(u.pos.x, u.pos.y));
            // Ground units face the corridor.
            e.q = glyder::attitude(static_cast<float>(std::atan2(-u.pos.x, -u.pos.y)));
            break;
        }
        }
        out.push_back(e);
        host.info(e.id, u.call, u.task);
    }

    for (const auto& s : sim.shots()) {
        if (s.outcome != Outcome::InFlight && s.impactAt < sim.time() - 1.5) continue;
        const bool burst = s.weapon == Weapon::GunBurst || s.weapon == Weapon::AaaBurst;
        if (burst) {
            if (s.outcome != Outcome::InFlight) continue;
            // Tracers: a short streak along the burst's path.
            wire::Effect t;
            t.kind = wire::EffectKind::Tracer;
            t.colour = glyder::hex(0xFFB060);
            t.a = v3(s.pos);
            t.b = v3(s.pos - s.vel * 0.12);
            t.width = 1.2f;
            fx.push_back(t);
            continue;
        }
        wire::EntityState e;
        e.id = kShotBase + static_cast<std::uint32_t>(s.id);
        e.shape = static_cast<std::uint16_t>(s.weapon == Weapon::LaserGuidedBomb ? kit::Shape::Bomb : kit::Shape::Missile);
        e.team = 2;
        e.tint = kit::palette::teams[2];
        e.pos = v3(s.pos);
        e.vel = v3(s.vel);
        e.q = glyder::alongVelocity(e.vel);
        e.flags = wire::kAlive | (s.outcome == Outcome::InFlight ? wire::kFiring : 0);
        out.push_back(e);
    }

    for (const auto& sat : sim.satellites()) {
        static std::map<std::string, std::uint32_t> ids;
        auto [it, fresh] = ids.try_emplace(sat.name, kSatBase + static_cast<std::uint32_t>(ids.size()));
        wire::EntityState e;
        e.id = it->second;
        e.shape = static_cast<std::uint16_t>(sat.relay ? kit::Shape::RelaySat : kit::Shape::ImagingSat);
        e.team = sat.relay ? 5 : 2;
        e.tint = kit::palette::teams[e.team];
        e.pos = v3(sat.pos);
        e.vel = v3(sat.vel);
        e.q = glyder::alongVelocity(e.vel);
        e.flags = wire::kAlive;
        out.push_back(e);
        host.info(e.id, sat.name, std::format("{} {:.0f} deg up", sat.relay ? "SAHAB relay" : "imaging satellite", sat.elevation));
    }

    for (const auto& b : sim.beams()) {
        const auto from = std::find_if(sim.units().begin(), sim.units().end(), [&](const Unit& u) { return u.id == b.from; });
        if (from == sim.units().end()) continue;
        wire::Effect l;
        const bool decoy = b.purpose.find("decoy") != std::string::npos;
        l.kind = wire::EffectKind::Laser;
        l.colour = section(b.colour);
        l.a = v3(from->pos);
        l.b = v3(b.to);
        if (decoy) l.b.z = static_cast<float>(ground(b.to.x, b.to.y)) + 0.5f;
        l.width = 0.6f;
        fx.push_back(l);
        if (decoy) {
            wire::Effect spot;
            spot.kind = wire::EffectKind::Spot;
            spot.colour = l.colour;
            spot.a = l.b;
            spot.width = 6.0f;
            fx.push_back(spot);
        }
    }
    // Thrown sound: rings spreading from each phantom source.
    const double phase = std::fmod(sim.time(), 2.0) / 2.0;
    for (const auto& p : sim.phantoms()) {
        for (int k = 0; k < 2; ++k) {
            const double ph = std::fmod(phase + k * 0.5, 1.0);
            wire::Effect r;
            r.kind = wire::EffectKind::Ring;
            r.colour = glyder::hex(0xFFC870);
            r.a = v3(p.pos);
            r.b = {static_cast<float>(40 + 260 * ph), 0, 0};
            r.width = 3.0f;
            r.intensity = static_cast<std::uint8_t>(255 * (1 - ph));
            fx.push_back(r);
        }
    }

    host.publish(sim.time(), std::move(out), std::move(fx));

    const auto& st = sim.stats();
    int kites = 0;
    for (const auto& u : sim.units()) kites += u.kind == Kind::Kite && u.alive;
    int relaysUp = 0;
    for (const auto& s : sim.skyTracks()) relaysUp += s.relay && s.elevation >= Sim::kRelayMaskDeg;
    const auto c = sim.convoyCenter();
    const double progress = std::clamp((c.y + 5200.0) / 10400.0, 0.0, 1.0);
    using wire::Tone;
    host.status({{"T+", std::format("{:02.0f}:{:02.0f}", std::floor(sim.time() / 60), std::fmod(sim.time(), 60.0)), Tone::Normal},
                 {"CONVOY", std::format("{:.0f}%  {} HITS", progress * 100, st.convoyHits), st.convoyHits ? Tone::Alert : Tone::Good},
                 {"KITES", std::format("{} / 24", kites), kites < 22 ? Tone::Warn : Tone::Good},
                 {"FIRED AT US", std::to_string(st.opforShots), Tone::Normal},
                 {"FIRED BY US", std::to_string(st.flockShots), Tone::Good},
                 {"NAV", std::format("{} {:.0f} M", shortNav(sim.navSource()), st.navErrorM), st.navErrorM > 100 ? Tone::Warn : Tone::Good},
                 {"RELAYS UP", std::to_string(relaysUp), relaysUp ? Tone::Good : Tone::Warn},
                 {"RECON", sim.reconOverhead() ? "OVERHEAD" : (sim.dispersed() ? "DISPERSED" : "CLEAR"),
                  sim.reconOverhead() ? Tone::Warn : Tone::Normal}});

    const auto& events = sim.events();
    for (std::size_t i = fromEvent; i < events.size(); ++i) {
        const auto& e = events[i];
        wire::EventKind k = wire::EventKind::Info;
        if (e.kind == "brain")
            k = wire::EventKind::Brain;
        else if (e.kind == "opfor" || e.kind == "shot")
            k = wire::EventKind::Opfor;
        else if (e.kind == "outcome")
            k = wire::EventKind::Outcome;
        else if (e.kind == "sky")
            k = wire::EventKind::Sky;
        else if (e.kind == "order")
            k = wire::EventKind::Order;
        if (e.kind == "shot") continue; // too many to read in a headset; the tracers say it
        host.event(k, e.text, e.t);
    }
    return events.size();
}

// --- Piloting a kite ---------------------------------------------------------------------------

namespace {
const Unit* findUnit(const Sim& sim, std::uint32_t id) {
    for (const auto& u : sim.units())
        if (static_cast<std::uint32_t>(u.id) == id) return &u;
    return nullptr;
}
} // namespace

std::optional<std::string> FlockPilot::refuse(std::uint32_t id) {
    if (id >= kSatBase) return "a satellite: SatLink tracks it, nobody flies it";
    if (id >= kShotBase) return "that is a shot fired at the flock";
    const Unit* u = findUnit(sim_, id);
    if (!u || !u->alive) return "that kite is down";
    if (u->kind != Kind::Kite) return "only the flock's kites can be flown from here";
    return std::nullopt;
}

std::optional<glyder::Pilot::Wait> FlockPilot::busy(std::uint32_t id) {
    const Unit* u = findUnit(sim_, id);
    if (!u) return std::nullopt;
    // A good stopping point: not in the middle of something the flock is relying on.
    if (u->breaking) return Wait{"finishing a break turn against an infrared missile", 2.0f};
    for (const auto& s : sim_.shots())
        if (s.outcome == Outcome::InFlight && s.target == u->id && s.weapon != Weapon::GunBurst && s.weapon != Weapon::AaaBurst)
            return Wait{"evading an inbound missile", static_cast<float>(std::max(0.0, s.impactAt - sim_.time()))};
    if (u->notchSince >= 0 && sim_.time() - u->notchSince < 3.0) return Wait{"holding a radar notch", 2.0f};
    for (const auto& b : sim_.beams())
        if (b.from == u->id) return Wait{"lasing: " + b.purpose, 1.0f};
    return std::nullopt;
}

glyder::Handling FlockPilot::take(std::uint32_t id) {
    const Unit* u = findUnit(sim_, id);
    glyder::Motion m;
    if (u) {
        m.pos = v3(u->pos);
        m.heading = static_cast<float>(std::atan2(u->vel.x, u->vel.y));
        m.speed = static_cast<float>(std::hypot(u->vel.x, u->vel.y));
        m.climb = static_cast<float>(u->vel.z);
    }
    motion_[id] = m;
    sim_.setManual(static_cast<int>(id), true);
    return {glyder::Handling::Model::Air, 18.0f, 46.0f, 0.7f, 8.0f, 6.0f};
}

void FlockPilot::drive(std::uint32_t id, const wire::Control& c, bool fresh, double dt) {
    auto it = motion_.find(id);
    if (it == motion_.end()) return;
    const glyder::Handling h{glyder::Handling::Model::Air, 18.0f, 46.0f, 0.7f, 8.0f, 6.0f};
    const float gz = static_cast<float>(ground(it->second.pos.x, it->second.pos.y));
    if (fresh) {
        const float axes[6] = {c.axes[0], c.axes[1], c.axes[2], c.axes[3], c.axes[4], c.axes[5]};
        glyder::step(it->second, h, axes, static_cast<float>(dt), gz + 30.0f);
    } else {
        glyder::hold(it->second, h, static_cast<float>(dt), gz + 30.0f);
    }
    const glyder::Vec3 v = it->second.velocity();
    sim_.steer(static_cast<int>(id), {it->second.pos.x, it->second.pos.y, it->second.pos.z}, {v.x, v.y, v.z});
}

void FlockPilot::give(std::uint32_t id) {
    motion_.erase(id);
    sim_.setManual(static_cast<int>(id), false);
}

bool FlockPilot::exists(std::uint32_t id) {
    const Unit* u = findUnit(sim_, id);
    return u && u->alive;
}

} // namespace ohflock
