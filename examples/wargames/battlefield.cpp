#include "battlefield.hpp"

#include <algorithm>
#include <array>
#include <charconv>

#include <gygax/core/charconv.hpp>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <numbers>

namespace wargames {

namespace {

using gygax::json::Value;

constexpr double kSubstep = 0.25;
constexpr int kSubstepsPerTick = 4;
constexpr double kFieldHalf = 1200.0;
constexpr double kMetersPerDegree = 111319.5;
constexpr double kHqRadar = 700.0;
constexpr double kServiceRadius = 35.0;
constexpr double kCargoMax = 500.0;
constexpr double kMaterielCap = 1500.0;
constexpr double kMaterielPerSecond = 9.0;

const std::array<Spec, 8> kSpecs = {{
    {"bunker", true, false, false, 4000.0, 0.0, 0.0, 0.0, 0.0, 0.0, 700.0, 0.0, 0.0},
    {"turret", true, false, true, 350.0, 0.0, 230.0, 70.0, 600.0, 3.0, 350.0, 0.0, 120.0},
    {"tank", true, false, true, 700.0, 8.0, 260.0, 90.0, 300.0, 1.5, 350.0, 0.0, 220.0},
    {"interceptor", true, true, true, 110.0, 45.0, 160.0, 45.0, 300.0, 3.0, 500.0, 5000.0, 90.0},
    {"supply-drone", true, true, false, 150.0, 30.0, 0.0, 0.0, 0.0, 0.0, 300.0, 3000.0, 70.0},
    {"alien-swarmer", false, false, true, 40.0, 16.0, 8.0, 22.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {"alien-bruiser", false, false, true, 900.0, 5.5, 14.0, 90.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {"alien-spitter", false, false, true, 120.0, 8.0, 130.0, 34.0, 0.0, 0.0, 0.0, 0.0, 0.0},
}};

const std::array<const char*, 8> kSectors = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

struct WaveShape {
    int swarmers;
    int bruisers;
    int spitters;
};

constexpr std::array<WaveShape, 8> kWaves = {
    {{30, 2, 4}, {45, 4, 8}, {60, 6, 12}, {80, 9, 16}, {100, 12, 22}, {125, 16, 28}, {150, 20, 34}, {180, 26, 42}}};

double waveTime(int wave) {
    return 10.0 + 35.0 * (wave - 1);
}

double round1(double v) {
    return std::round(v * 10.0) / 10.0;
}

Value vecJson(const Vec& v) {
    Value o = Value::object();
    o["x"] = round1(v.x);
    o["y"] = round1(v.y);
    return o;
}

Vec toward(const Vec& from, const Vec& to, double step) {
    const double d = distance(from, to);
    if (d <= 1e-9 || step >= d) return to;
    return {from.x + (to.x - from.x) / d * step, from.y + (to.y - from.y) / d * step};
}

Vec clampField(Vec v) {
    v.x = std::clamp(v.x, -kFieldHalf, kFieldHalf);
    v.y = std::clamp(v.y, -kFieldHalf, kFieldHalf);
    return v;
}

int targetScore(Kind kind) {
    switch (kind) {
    case Kind::Spitter: return 3;
    case Kind::Bruiser: return 2;
    default: return 1;
    }
}

} // namespace

const Spec& specOf(Kind kind) {
    return kSpecs[static_cast<std::size_t>(kind)];
}

const char* roleOf(Kind kind) {
    switch (kind) {
    case Kind::Bunker: return "commander";
    case Kind::Turret: return "turret";
    case Kind::Tank: return "tank";
    case Kind::Interceptor: return "interceptor";
    case Kind::Supply: return "supply";
    default: return "alien";
    }
}

double distance(const Vec& a, const Vec& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

const char* sectorOf(const Vec& origin, const Vec& p) {
    const double bearing = std::atan2(p.x - origin.x, p.y - origin.y);
    const double turns = std::fmod(bearing / (2.0 * std::numbers::pi) + 1.0, 1.0);
    const auto index = static_cast<std::size_t>(std::floor(turns * 8.0 + 0.5)) % 8;
    return kSectors[index];
}

Vec sectorPoint(const std::string& sector, double radius) {
    for (std::size_t i = 0; i < kSectors.size(); ++i) {
        if (sector == kSectors[i]) {
            const double bearing = static_cast<double>(i) * std::numbers::pi / 4.0;
            return {std::sin(bearing) * radius, std::cos(bearing) * radius};
        }
    }
    return {};
}

gygax::logistics::Point Battlefield::toPoint(const Vec& v) {
    return {v.y / kMetersPerDegree, v.x / kMetersPerDegree};
}

Battlefield::Battlefield(Options options) : options_(options), rng_(options.seed), ledger_() {
    using gygax::json::parse;
    for (const auto* text : {R"({"sku":"interceptor","kind":"quadcopter","power":"battery","max_range_m":5000,"cruise_mps":45})",
                             R"({"sku":"supply-drone","kind":"quadcopter","power":"battery","max_range_m":3000,"cruise_mps":30})"}) {
        ledger_.putModel(*parse(text));
    }
    const struct {
        const char* id;
        const char* kind;
        double x;
        double y;
        const char* services;
    } sites[] = {{"hq", "depot", 0.0, 0.0, R"(["battery","ammo"])"},
                 {"pad-e", "refuel", 300.0, 0.0, R"(["battery"])"},
                 {"pad-w", "refuel", -300.0, 0.0, R"(["battery"])"},
                 {"pad-n", "refuel", 0.0, 300.0, R"(["battery"])"},
                 {"pad-s", "refuel", 0.0, -300.0, R"(["battery"])"}};
    for (const auto& s : sites) {
        Value spec = Value::object();
        spec["id"] = s.id;
        spec["kind"] = s.kind;
        const auto p = toPoint({s.x, s.y});
        spec["latitude"] = p.latitude;
        spec["longitude"] = p.longitude;
        spec["services"] = *parse(s.services);
        ledger_.putSite(spec);
    }
    gygax::neuro::LifParams lif;
    hive_ = std::make_unique<gygax::neuro::Network>(0.5, options.seed ^ 0x9E3779B97F4A7C15ULL);
    const auto losses = hive_->addPoisson("losses", 4, 0.0);
    const auto rage = hive_->addLif("rage", 16, lif);
    gygax::neuro::ConnectionSpec spec;
    spec.kind = gygax::neuro::Connectivity::AllToAll;
    spec.weightMean = 6.0;
    spec.delayMs = 1.0;
    hive_->connect(losses, rage, spec);

    spawn(Kind::Bunker, {0.0, 0.0}, 0).name = "HQ";
    for (int i = 0; i < 8; ++i) {
        const double bearing = static_cast<double>(i) * std::numbers::pi / 4.0;
        const double radius = i % 2 == 0 ? 75.0 : 115.0;
        spawn(Kind::Turret, {std::sin(bearing) * radius, std::cos(bearing) * radius}, 0);
    }
    for (int i = 0; i < 3; ++i) spawn(Kind::Tank, {-30.0 + 30.0 * i, -20.0}, 0);
    for (int i = 0; i < 4; ++i) spawn(Kind::Interceptor, {-15.0 + 10.0 * i, 12.0}, 0);
    for (int i = 0; i < 2; ++i) spawn(Kind::Supply, {-6.0 + 12.0 * i, 24.0}, 0);
    note("last stand begins: 1 bunker, 8 turrets, 3 tanks, 4 interceptors, 2 supply drones");
}

Entity& Battlefield::spawn(Kind kind, Vec pos, int wave) {
    const auto& spec = specOf(kind);
    Entity e;
    e.id = nextId_++;
    e.kind = kind;
    e.pos = pos;
    e.hp = spec.hp;
    e.ammo = spec.ammo;
    e.cargo = kind == Kind::Supply ? kCargoMax : 0.0;
    e.wave = wave;
    if (spec.human) {
        const char* prefix = kind == Kind::Turret        ? "T"
                             : kind == Kind::Tank        ? "K"
                             : kind == Kind::Interceptor ? "D"
                             : kind == Kind::Supply      ? "S"
                                                         : "HQ";
        const int n = ++counters_[std::string("name:") + prefix];
        e.name = kind == Kind::Bunker ? "HQ" : std::format("{}{}", prefix, n);
        gygax::json::Value req = gygax::json::Value::object();
        req["delta"] = 1;
        req["sku"] = spec.sku;
        req["site"] = "hq";
        req["reason"] = wave == 0 ? "deployed" : "produced";
        if (spec.air) {
            req["attrs"] = *gygax::json::parse(R"({"drone":"quadcopter","power":"battery"})");
        }
        auto out = ledger_.record(req);
        if (out.ok) e.guid = out.body.find("guids")->asArray().front().asString();
        newHumans_.push_back(e.id);
        if (wave != 0) ++stats_.humansBuilt;
    } else {
        e.name = std::format("A{}", e.id);
    }
    entities_.push_back(std::move(e));
    return entities_.back();
}

void Battlefield::note(std::string line) {
    narrative_.push_back(std::format("[{:6.1f}s] {}", time_, std::move(line)));
}

void Battlefield::spawnWave(int wave) {
    const auto& shape = kWaves[static_cast<std::size_t>(std::min<int>(wave, static_cast<int>(kWaves.size())) - 1)];
    auto scaled = [&](int base) { return static_cast<int>(std::lround(base * options_.difficulty)); };
    const std::array<int, 3> counts = {scaled(shape.swarmers), scaled(shape.bruisers), scaled(shape.spitters)};
    const std::array<Kind, 3> kinds = {Kind::Swarmer, Kind::Bruiser, Kind::Spitter};
    const double bearing = rng_.uniform() * 2.0 * std::numbers::pi;
    const bool split = wave >= 4;
    std::string summary;
    for (std::size_t k = 0; k < kinds.size(); ++k) {
        if (counts[k] <= 0) continue;
        gygax::json::Value req = gygax::json::Value::object();
        req["delta"] = counts[k];
        req["sku"] = specOf(kinds[k]).sku;
        req["reason"] = "invasion";
        req["attrs"] = *gygax::json::parse(std::format(R"({{"wave":"{}"}})", wave));
        auto out = ledger_.record(req);
        const auto guids = out.ok ? out.body.find("guids")->asArray() : std::vector<Value>{};
        for (int i = 0; i < counts[k]; ++i) {
            const double side = split && i % 2 == 1 ? std::numbers::pi : 0.0;
            const double angle = bearing + side + (rng_.uniform() - 0.5) * 0.7;
            const double radius = 950.0 + rng_.uniform() * 100.0;
            auto& e = spawn(kinds[k], {std::sin(angle) * radius, std::cos(angle) * radius}, wave);
            if (static_cast<std::size_t>(i) < guids.size()) e.guid = guids[static_cast<std::size_t>(i)].asString();
            ++stats_.aliensSpawned;
        }
        summary += std::format("{} {}{}", counts[k], specOf(kinds[k]).sku + 6, k + 1 < kinds.size() ? ", " : "");
    }
    outcome_.wavesSpawned = wave;
    note(std::format("WAVE {} from {}{}: {}", wave, sectorOf({0, 0}, {std::sin(bearing), std::cos(bearing)}),
                     split ? " and the opposite flank" : "", summary));
}

void Battlefield::beginTick() {
    std::lock_guard lock(mutex_);
    ++tick_;
    casualtiesThisTick_ = 0;
    materiel_ = std::min(kMaterielCap, materiel_ + kMaterielPerSecond);
    for (auto& e : entities_) e.orders = {};
    while (nextWave_ <= options_.waves && time_ >= waveTime(nextWave_)) spawnWave(nextWave_++);
}

void Battlefield::kill(Entity& e, const char* reason) {
    if (!e.alive) return;
    e.alive = false;
    e.hp = 0.0;
    const auto& spec = specOf(e.kind);
    if (!e.guid.empty()) {
        gygax::json::Value req = gygax::json::Value::object();
        req["delta"] = -1;
        req["sku"] = spec.sku;
        req["reason"] = spec.human ? "lost" : "killed";
        gygax::json::Value guids = gygax::json::Value::array();
        guids.push(e.guid);
        req["guids"] = std::move(guids);
        ledger_.record(req);
    }
    if (spec.human) {
        ++stats_.humansLost;
        ++counters_[std::string("lost:") + spec.sku];
        retired_.push_back(e.name);
        note(std::format("{} {} destroyed ({})", spec.sku, e.name, reason));
    } else {
        ++stats_.aliensKilled;
        ++casualtiesThisTick_;
        if (e.kind == Kind::Bruiser) note(std::format("bruiser {} down", e.name));
    }
}

void Battlefield::substep(double dt) {
    std::vector<double> damage(entities_.size(), 0.0);
    auto indexOf = [&](std::uint32_t id) { return static_cast<std::size_t>(id - 1); };

    for (auto& a : entities_) {
        if (!a.alive || specOf(a.kind).human) continue;
        const auto& spec = specOf(a.kind);
        Entity* best = nullptr;
        double bestD = 1e18;
        for (auto& h : entities_) {
            if (!h.alive || !specOf(h.kind).human) continue;
            if (specOf(h.kind).air && a.kind != Kind::Spitter) continue;
            const double d = distance(a.pos, h.pos);
            if (d < bestD) {
                bestD = d;
                best = &h;
            }
        }
        if (best == nullptr) continue;
        a.engaged = best->id;
        const double reach = spec.range * (a.kind == Kind::Spitter ? 0.9 : 0.8);
        if (bestD > reach) {
            const double boost = a.kind == Kind::Swarmer ? rage_ : 1.0;
            a.pos = toward(a.pos, best->pos, spec.speed * boost * dt);
        } else {
            damage[indexOf(best->id)] += spec.dps * dt;
            stats_.damageTaken += spec.dps * dt;
        }
    }

    for (auto& h : entities_) {
        if (!h.alive || !specOf(h.kind).human) continue;
        const auto& spec = specOf(h.kind);
        if (spec.armed && h.ammo > 0.0 && directives_.weapons != "hold") {
            Entity* target = nullptr;
            if (directives_.focus != 0) {
                Entity* focused = mutableFind(directives_.focus);
                if (focused != nullptr && focused->alive && distance(h.pos, focused->pos) <= spec.range) target = focused;
            }
            if (target == nullptr && h.orders.fire != 0) {
                Entity* ordered = mutableFind(h.orders.fire);
                if (ordered != nullptr && ordered->alive && !specOf(ordered->kind).human && distance(h.pos, ordered->pos) <= spec.range)
                    target = ordered;
            }
            if (target == nullptr) {
                double bestD = spec.range;
                for (auto& a : entities_) {
                    if (!a.alive || specOf(a.kind).human) continue;
                    const double d = distance(h.pos, a.pos);
                    if (directives_.weapons == "tight" && targetScore(a.kind) < 2 && d > spec.range * 0.5) continue;
                    if (d <= bestD) {
                        bestD = d;
                        target = &a;
                    }
                }
            }
            if (target != nullptr) {
                const double dealt = spec.dps * dt * (0.8 + 0.4 * rng_.uniform());
                damage[indexOf(target->id)] += dealt;
                stats_.damageDealt += dealt;
                const double used = std::min(h.ammo, spec.ammoPerSecond * dt);
                h.ammo -= used;
                stats_.shotsFired += static_cast<std::uint64_t>(std::lround(used));
            }
        }

        if (h.kind == Kind::Supply && h.orders.resupply != 0) {
            if (const Entity* t = find(h.orders.resupply); t != nullptr && t->alive)
                h.orders.destination = t->pos, h.orders.hasDestination = true;
        }
        if (spec.speed > 0.0 && h.orders.hasDestination) {
            const Vec next = toward(h.pos, clampField(h.orders.destination), spec.speed * dt);
            const double moved = distance(h.pos, next);
            h.pos = next;
            if (spec.air) h.fuel -= moved / spec.rangeMeters;
        } else if (spec.air) {
            h.fuel -= 0.0;
        }

        if (spec.air) {
            for (const auto& site : ledger_.sites()) {
                const Vec sp{site.position.longitude * kMetersPerDegree, site.position.latitude * kMetersPerDegree};
                if (distance(h.pos, sp) <= kServiceRadius) {
                    h.fuel = std::min(1.0, h.fuel + 0.25 * dt);
                    if (site.id == "hq") {
                        if (h.kind == Kind::Interceptor) h.ammo = std::min(spec.ammo, h.ammo + 40.0 * dt);
                        if (h.kind == Kind::Supply) h.cargo = std::min(kCargoMax, h.cargo + 150.0 * dt);
                    }
                }
            }
            if (h.fuel <= 0.0) damage[indexOf(h.id)] += 1e9;
        }
        if (h.kind == Kind::Tank && distance(h.pos, {0.0, 0.0}) <= kServiceRadius + 15.0) h.ammo = std::min(spec.ammo, h.ammo + 25.0 * dt);

        if (h.kind == Kind::Supply && h.cargo > 0.0) {
            for (auto& t : entities_) {
                if (!t.alive || !specOf(t.kind).human || !specOf(t.kind).armed || t.id == h.id) continue;
                const auto& ts = specOf(t.kind);
                if (t.ammo >= ts.ammo - 1.0 || distance(h.pos, t.pos) > kServiceRadius) continue;
                const double give = std::min({h.cargo, ts.ammo - t.ammo, 200.0 * dt});
                t.ammo += give;
                h.cargo -= give;
                stats_.ammoDelivered += give;
            }
        }
    }

    for (auto& e : entities_) {
        const double d = damage[indexOf(e.id)];
        if (!e.alive || d <= 0.0) continue;
        e.hp -= d;
        if (e.hp <= 0.0) kill(e, specOf(e.kind).air && e.fuel <= 0.0 ? "fuel-exhausted" : "killed");
    }
}

void Battlefield::endTick() {
    std::lock_guard lock(mutex_);
    for (int i = 0; i < kSubstepsPerTick; ++i) substep(kSubstep);
    time_ += 1.0;

    hive_->setRate(0, std::min(400.0, static_cast<double>(casualtiesThisTick_) * 60.0));
    hive_->clearSpikes();
    hive_->run(1000.0);
    rage_ = 1.0 + std::min(0.5, hive_->meanRateHz(1) / 120.0);

    if (bunkerHp() <= 0.0) {
        outcome_ = {true, false, time_, outcome_.wavesSpawned};
        note("the bunker has fallen");
    } else if (options_.waves > 0 && nextWave_ > options_.waves && aliveAliens() == 0) {
        outcome_ = {true, true, time_, outcome_.wavesSpawned};
        note("the last alien is dead");
    } else if (time_ >= options_.seconds) {
        outcome_ = {true, bunkerHp() > 0.0, time_, outcome_.wavesSpawned};
        note("time is up");
    }
}

Entity* Battlefield::mutableFind(std::uint32_t id) {
    if (id == 0 || id > entities_.size()) return nullptr;
    return &entities_[id - 1];
}

const Entity* Battlefield::find(std::uint32_t id) const {
    if (id == 0 || id > entities_.size()) return nullptr;
    return &entities_[id - 1];
}

const Entity* Battlefield::findByName(const std::string& name) const {
    for (const auto& e : entities_) {
        if (e.alive && specOf(e.kind).human && e.name == name) return &e;
    }
    return nullptr;
}

std::vector<std::uint32_t> Battlefield::livingHumanIds() const {
    std::vector<std::uint32_t> out;
    for (const auto& e : entities_) {
        if (e.alive && specOf(e.kind).human) out.push_back(e.id);
    }
    return out;
}

std::vector<std::uint32_t> Battlefield::newHumanIds() {
    std::lock_guard lock(mutex_);
    return std::exchange(newHumans_, {});
}

std::vector<std::string> Battlefield::takeRetired() {
    std::lock_guard lock(mutex_);
    return std::exchange(retired_, {});
}

std::vector<std::string> Battlefield::takeNarrative() {
    std::lock_guard lock(mutex_);
    return std::exchange(narrative_, {});
}

int Battlefield::alive(Kind kind) const {
    return static_cast<int>(std::ranges::count_if(entities_, [&](const Entity& e) { return e.alive && e.kind == kind; }));
}

int Battlefield::aliveAliens() const {
    return static_cast<int>(std::ranges::count_if(entities_, [](const Entity& e) { return e.alive && !specOf(e.kind).human; }));
}

double Battlefield::bunkerHp() const {
    return entities_.empty() ? 0.0 : entities_.front().hp;
}

Value Battlefield::observe(std::uint32_t id) const {
    const Entity* self = find(id);
    Value out = Value::object();
    if (self == nullptr) return out;
    const auto& spec = specOf(self->kind);
    out["unit"] = self->name;
    out["role"] = roleOf(self->kind);
    out["tick"] = tick_;
    out["time"] = time_;
    Value me = vecJson(self->pos);
    me["hp_frac"] = round1(self->hp / spec.hp * 100.0) / 100.0;
    me["ammo_frac"] = spec.ammo > 0.0 ? round1(self->ammo / spec.ammo * 100.0) / 100.0 : 1.0;
    me["fuel"] = round1(self->fuel * 100.0) / 100.0;
    me["cargo"] = round1(self->cargo);
    me["range"] = spec.range;
    me["max_range_m"] = spec.rangeMeters;
    out["self"] = std::move(me);

    struct Seen {
        const Entity* e;
        double d;
    };
    std::vector<Seen> seen;
    std::map<std::string, int> perSector;
    for (const auto& a : entities_) {
        if (!a.alive || specOf(a.kind).human) continue;
        const double d = distance(self->pos, a.pos);
        if (d <= spec.sensor || distance({0.0, 0.0}, a.pos) <= kHqRadar) {
            seen.push_back({&a, d});
            ++perSector[sectorOf({0.0, 0.0}, a.pos)];
        }
    }
    std::ranges::sort(seen, [](const Seen& l, const Seen& r) { return l.d != r.d ? l.d < r.d : l.e->id < r.e->id; });
    Value contacts = Value::array();
    for (std::size_t i = 0; i < seen.size() && i < 24; ++i) {
        Value c = vecJson(seen[i].e->pos);
        c["id"] = seen[i].e->id;
        c["kind"] = std::string(specOf(seen[i].e->kind).sku).substr(6);
        c["hp"] = round1(seen[i].e->hp);
        c["dist"] = round1(seen[i].d);
        c["score"] = targetScore(seen[i].e->kind);
        contacts.push(std::move(c));
    }
    out["contacts"] = std::move(contacts);
    out["visible"] = seen.size();
    Value sectors = Value::object();
    for (const auto& [name, n] : perSector) sectors[name] = n;
    out["sectors"] = std::move(sectors);

    if (self->kind == Kind::Supply) {
        struct Need {
            const Entity* e;
            double frac;
        };
        std::vector<Need> needs;
        for (const auto& t : entities_) {
            if (!t.alive || !specOf(t.kind).human || !specOf(t.kind).armed) continue;
            const double frac = t.ammo / specOf(t.kind).ammo;
            if (frac < 0.6) needs.push_back({&t, frac});
        }
        std::ranges::sort(needs, [](const Need& l, const Need& r) { return l.frac != r.frac ? l.frac < r.frac : l.e->id < r.e->id; });
        Value list = Value::array();
        for (std::size_t i = 0; i < needs.size() && i < 6; ++i) {
            Value n = vecJson(needs[i].e->pos);
            n["unit"] = needs[i].e->name;
            n["ammo_frac"] = round1(needs[i].frac * 100.0) / 100.0;
            n["dist"] = round1(distance(self->pos, needs[i].e->pos));
            list.push(std::move(n));
        }
        out["needs_ammo"] = std::move(list);
    }

    if (!directives_.rally.empty()) {
        Value r = vecJson(sectorPoint(directives_.rally, 160.0));
        r["sector"] = directives_.rally;
        out["rally"] = std::move(r);
    }
    Value orders = Value::object();
    orders["weapons"] = directives_.weapons;
    orders["priority"] = directives_.priority;
    orders["drones"] = directives_.drones;
    orders["focus"] = directives_.focus;
    orders["rally_by_human"] = directives_.rallyHuman;
    Value notes = Value::array();
    for (const auto& n : directives_.notes) notes.push(n);
    orders["notes"] = std::move(notes);
    out["directives"] = std::move(orders);
    Value siteList = Value::array();
    for (const auto& s : ledger_.sites()) {
        const Vec sp{s.position.longitude * kMetersPerDegree, s.position.latitude * kMetersPerDegree};
        Value item = vecJson(sp);
        item["id"] = s.id;
        item["kind"] = s.kind;
        item["dist"] = round1(distance(self->pos, sp));
        siteList.push(std::move(item));
    }
    out["sites"] = std::move(siteList);
    return out;
}

Value Battlefield::commanderView() const {
    Value out = Value::object();
    out["time"] = time_;
    out["tick"] = tick_;
    out["materiel"] = round1(materiel_);
    out["bunker_hp_frac"] = round1(bunkerHp() / specOf(Kind::Bunker).hp * 100.0) / 100.0;
    out["aliens_alive"] = aliveAliens();
    out["waves_spawned"] = outcome_.wavesSpawned;
    out["waves_total"] = options_.waves;
    out["rally"] = directives_.rally;
    out["autoproduce"] = directives_.autoProduce;
    Value queue = Value::array();
    for (const auto& q : directives_.produceQueue) queue.push(q);
    out["produce_queue"] = std::move(queue);
    out["weapons"] = directives_.weapons;
    Value inventory = Value::object();
    for (const auto kind : {Kind::Turret, Kind::Tank, Kind::Interceptor, Kind::Supply}) inventory[specOf(kind).sku] = alive(kind);
    out["inventory"] = std::move(inventory);
    Value costs = Value::object();
    for (const auto kind : {Kind::Tank, Kind::Interceptor, Kind::Supply}) costs[specOf(kind).sku] = specOf(kind).cost;
    out["costs"] = std::move(costs);
    return out;
}

Value Battlefield::plan(std::uint32_t id, const Vec& target, bool roundTrip) const {
    const Entity* self = find(id);
    Value out = Value::object();
    if (self == nullptr) return out;
    const auto& spec = specOf(self->kind);
    gygax::logistics::PlanRequest req;
    req.origin = toPoint(self->pos);
    req.waypoints = {toPoint(target)};
    if (roundTrip) req.returnTo = toPoint({0.0, 0.0});
    req.maxRangeM = spec.rangeMeters;
    req.fuelFraction = std::clamp(self->fuel, 0.0, 1.0);
    req.cruiseMps = spec.speed;
    req.power = "battery";
    out = gygax::logistics::planRoute(req, ledger_.sites());
    Value route = Value::array();
    if (const auto* legs = out.find("legs")) {
        for (const auto& leg : legs->asArray()) {
            const auto* to = leg.find("to");
            if (to == nullptr) continue;
            Value stop = Value::object();
            stop["x"] = round1(to->getDouble("longitude") * kMetersPerDegree);
            stop["y"] = round1(to->getDouble("latitude") * kMetersPerDegree);
            stop["type"] = leg.getString("type");
            route.push(std::move(stop));
        }
    }
    Value trimmed = Value::object();
    trimmed["feasible"] = out.getBool("feasible");
    trimmed["refuel_stops"] = out.getInt("refuel_stops");
    trimmed["total_distance_m"] = round1(out.getDouble("total_distance_m"));
    trimmed["route"] = std::move(route);
    if (!out.getBool("feasible")) trimmed["reason"] = out.getString("reason");
    return trimmed;
}

std::string Battlefield::applyOrders(std::uint32_t id, const Value& orders) {
    std::lock_guard lock(mutex_);
    Entity* e = mutableFind(id);
    if (e == nullptr || !e->alive || !specOf(e->kind).human) throw std::runtime_error("no such living unit");
    if (!orders.isArray()) throw std::runtime_error("orders must be an array");
    const auto& spec = specOf(e->kind);
    Orders next;
    int accepted = 0;
    for (const auto& o : orders.asArray()) {
        if (const auto* fire = o.find("fire")) {
            if (!spec.armed) throw std::runtime_error("this unit has no weapon");
            const Entity* t = find(static_cast<std::uint32_t>(fire->asInt()));
            if (t == nullptr || !t->alive || specOf(t->kind).human) throw std::runtime_error("fire target is not a living hostile");
            next.fire = t->id;
        } else if (const auto* move = o.find("move")) {
            if (spec.speed <= 0.0) throw std::runtime_error("this unit cannot move");
            if (!move->isArray() || move->size() != 2) throw std::runtime_error("move needs [x, y]");
            next.hasDestination = true;
            next.destination = clampField({move->asArray()[0].asDouble(), move->asArray()[1].asDouble()});
        } else if (const auto* supply = o.find("resupply")) {
            if (e->kind != Kind::Supply) throw std::runtime_error("only supply drones can resupply");
            const Entity* t = findByName(supply->asString());
            if (t == nullptr || !specOf(t->kind).armed) throw std::runtime_error("no such armed friendly unit");
            next.resupply = t->id;
        } else {
            throw std::runtime_error("unknown order");
        }
        ++accepted;
    }
    e->orders = next;
    Value out = Value::object();
    out["ok"] = true;
    out["accepted"] = accepted;
    return out.dump();
}

std::string Battlefield::produce(const std::string& kindName) {
    std::lock_guard lock(mutex_);
    Kind kind;
    if (kindName == "interceptor")
        kind = Kind::Interceptor;
    else if (kindName == "tank")
        kind = Kind::Tank;
    else if (kindName == "supply-drone")
        kind = Kind::Supply;
    else
        throw std::runtime_error("cannot produce '" + kindName + "'");
    const auto& spec = specOf(kind);
    if (materiel_ < spec.cost) throw std::runtime_error("not enough materiel");
    if (alive(kind) >= 12) throw std::runtime_error("production cap reached");
    materiel_ -= spec.cost;
    if (!directives_.produceQueue.empty() && directives_.produceQueue.front() == kindName)
        directives_.produceQueue.erase(directives_.produceQueue.begin());
    const double jitter = static_cast<double>(counters_["build"]++ % 5) * 6.0;
    auto& e = spawn(kind, {-12.0 + jitter, -34.0}, tick_ + 1000);
    e.wave = 0;
    note(std::format("factory delivers {} {}", spec.sku, e.name));
    Value out = Value::object();
    out["built"] = e.name;
    out["materiel"] = round1(materiel_);
    return out.dump();
}

void Battlefield::designate(const std::string& sector) {
    std::lock_guard lock(mutex_);
    if (directives_.rallyHuman) return;
    const std::string next = sector == "IDLE" ? std::string() : sector;
    if (next != directives_.rally && !next.empty()) note("command designates sector " + next);
    directives_.rally = next;
}

Directives Battlefield::directives() const {
    std::lock_guard lock(mutex_);
    return directives_;
}

std::string Battlefield::status() const {
    std::lock_guard lock(mutex_);
    return std::format("t={:.0f}s wave {}/{} aliens={} bunker={:.0f}% T={} K={} D={} S={} materiel={:.0f} weapons={} priority={} drones={} "
                       "rally={}{} focus={}",
                       time_, outcome_.wavesSpawned, options_.waves, aliveAliens(), bunkerHp() / specOf(Kind::Bunker).hp * 100.0,
                       alive(Kind::Turret), alive(Kind::Tank), alive(Kind::Interceptor), alive(Kind::Supply), materiel_,
                       directives_.weapons, directives_.priority, directives_.drones, directives_.rally.empty() ? "-" : directives_.rally,
                       directives_.rallyHuman ? "(human)" : "", directives_.focus);
}

std::string Battlefield::directive(const std::string& line) {
    std::lock_guard lock(mutex_);
    std::vector<std::string> words;
    std::string word;
    for (const char ch : line) {
        if (ch == ' ' || ch == '\t') {
            if (!word.empty()) words.push_back(std::exchange(word, {}));
        } else {
            word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    if (!word.empty()) words.push_back(word);
    if (words.empty()) return "error: empty directive";
    const auto& cmd = words[0];
    const std::string arg = words.size() > 1 ? words[1] : std::string();
    if (cmd == "rally") {
        if (arg == "auto" || arg == "clear") {
            directives_.rallyHuman = false;
            directives_.rally.clear();
            return "ok: rally point returned to agent consensus";
        }
        std::string sector = arg;
        for (auto& ch : sector) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (sectorPoint(sector, 1.0).x == 0.0 && sectorPoint(sector, 1.0).y == 0.0)
            return "error: sector must be N NE E SE S SW W NW or auto";
        directives_.rally = sector;
        directives_.rallyHuman = true;
        note("HUMAN COMMAND: rally " + sector);
        return "ok: mobile units rally to sector " + sector;
    }
    if (cmd == "priority") {
        if (arg != "threat" && arg != "spitter" && arg != "bruiser" && arg != "swarmer" && arg != "nearest")
            return "error: priority is threat, spitter, bruiser, swarmer or nearest";
        directives_.priority = arg;
        note("HUMAN COMMAND: priority " + arg);
        return "ok: target priority " + arg;
    }
    if (cmd == "weapons") {
        if (arg != "free" && arg != "tight" && arg != "hold") return "error: weapons is free, tight or hold";
        directives_.weapons = arg;
        note("HUMAN COMMAND: weapons " + arg);
        return "ok: weapons " + arg;
    }
    if (cmd == "drones") {
        if (arg != "hunt" && arg != "recall") return "error: drones is hunt or recall";
        directives_.drones = arg;
        note("HUMAN COMMAND: drones " + arg);
        return "ok: drones " + arg;
    }
    if (cmd == "focus") {
        if (arg == "off" || arg == "clear") {
            directives_.focus = 0;
            return "ok: focus fire cleared";
        }
        std::uint32_t id = 0;
        const auto [end, ec] = gygax::fromChars(arg.data(), arg.data() + arg.size(), id);
        const Entity* t = find(id);
        if (ec != std::errc() || end != arg.data() + arg.size() || t == nullptr || !t->alive || specOf(t->kind).human)
            return "error: focus needs the id of a living hostile";
        directives_.focus = id;
        note(std::format("HUMAN COMMAND: focus fire on {}", t->name));
        return "ok: all weapons focus " + t->name;
    }
    if (cmd == "produce") {
        if (arg != "interceptor" && arg != "tank" && arg != "supply-drone") return "error: produce interceptor, tank or supply-drone";
        directives_.produceQueue.push_back(arg);
        note("HUMAN COMMAND: build " + arg);
        return "ok: queued " + arg + " for the commander agent";
    }
    if (cmd == "autoproduce") {
        if (arg != "on" && arg != "off") return "error: autoproduce is on or off";
        directives_.autoProduce = arg == "on";
        return "ok: autoproduce " + arg;
    }
    if (cmd == "note") {
        const auto at = line.find(' ');
        std::string text = at == std::string::npos ? std::string() : line.substr(at + 1);
        if (text.empty() || text.size() > 200) return "error: note needs 1-200 characters";
        directives_.notes.push_back(std::move(text));
        if (directives_.notes.size() > 3) directives_.notes.erase(directives_.notes.begin());
        return "ok: note delivered to every agent";
    }
    return "error: unknown directive '" + cmd + "'";
}

std::uint64_t Battlefield::digest() const {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&](std::int64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= static_cast<std::uint64_t>(v >> (i * 8)) & 0xFFU;
            h *= 1099511628211ULL;
        }
    };
    mix(static_cast<std::int64_t>(time_ * 100.0));
    mix(static_cast<std::int64_t>(entities_.size()));
    for (const auto& e : entities_) {
        mix(e.id);
        mix(e.alive ? 1 : 0);
        mix(std::llround(e.pos.x * 10.0));
        mix(std::llround(e.pos.y * 10.0));
        mix(std::llround(e.hp * 10.0));
        mix(std::llround(e.ammo * 10.0));
    }
    mix(static_cast<std::int64_t>(stats_.shotsFired));
    return h;
}

std::string Battlefield::render() const {
    constexpr int cols = 71;
    constexpr int rows = 33;
    std::vector<std::string> grid(rows, std::string(cols, '.'));
    auto put = [&](const Vec& p, char c, int priority, std::vector<int>& prio) {
        const int col = static_cast<int>(std::lround((p.x + 1100.0) / 2200.0 * (cols - 1)));
        const int row = static_cast<int>(std::lround((1100.0 - p.y) / 2200.0 * (rows - 1)));
        if (col < 0 || col >= cols || row < 0 || row >= rows) return;
        const auto cell = static_cast<std::size_t>(row * cols + col);
        if (prio[cell] >= priority) return;
        prio[cell] = priority;
        grid[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] = c;
    };
    std::vector<int> prio(static_cast<std::size_t>(cols * rows), 0);
    for (const auto& e : entities_) {
        if (!e.alive) continue;
        switch (e.kind) {
        case Kind::Bunker: put(e.pos, 'H', 9, prio); break;
        case Kind::Turret: put(e.pos, 'T', 8, prio); break;
        case Kind::Tank: put(e.pos, 'K', 7, prio); break;
        case Kind::Interceptor: put(e.pos, 'i', 6, prio); break;
        case Kind::Supply: put(e.pos, 's', 5, prio); break;
        case Kind::Bruiser: put(e.pos, 'B', 4, prio); break;
        case Kind::Spitter: put(e.pos, 'x', 3, prio); break;
        case Kind::Swarmer: put(e.pos, 'a', 2, prio); break;
        }
    }
    std::string out;
    for (const auto& row : grid) out += row + "\n";
    return out;
}

}
