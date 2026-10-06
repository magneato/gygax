#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <gygax/core/json.hpp>
#include <gygax/logistics/ledger.hpp>
#include <gygax/neuro/network.hpp>

namespace wargames {

enum class Kind { Bunker, Turret, Tank, Interceptor, Supply, Swarmer, Bruiser, Spitter };

struct Vec {
    double x = 0.0;
    double y = 0.0;
};

struct Spec {
    const char* sku;
    bool human;
    bool air;
    bool armed;
    double hp;
    double speed;
    double range;
    double dps;
    double ammo;
    double ammoPerSecond;
    double sensor;
    double rangeMeters;
    double cost;
};

const Spec& specOf(Kind kind);
const char* roleOf(Kind kind);
double distance(const Vec& a, const Vec& b);
const char* sectorOf(const Vec& origin, const Vec& p);
Vec sectorPoint(const std::string& sector, double radius);

struct Orders {
    std::uint32_t fire = 0;
    bool hasDestination = false;
    Vec destination;
    std::uint32_t resupply = 0;
};

struct Entity {
    std::uint32_t id = 0;
    Kind kind = Kind::Turret;
    std::string name;
    std::string guid;
    Vec pos;
    double hp = 0.0;
    double ammo = 0.0;
    double fuel = 1.0;
    double cargo = 0.0;
    bool alive = true;
    int wave = 0;
    std::uint32_t engaged = 0;
    Orders orders;
};

struct Directives {
    std::string weapons = "free";
    std::string priority = "threat";
    std::string drones = "hunt";
    std::string rally;
    bool rallyHuman = false;
    bool autoProduce = true;
    std::uint32_t focus = 0;
    std::vector<std::string> produceQueue;
    std::vector<std::string> notes;
};

struct Options {
    std::uint64_t seed = 7;
    double seconds = 240.0;
    int waves = 6;
    double difficulty = 2.5;
};

struct Stats {
    std::uint64_t aliensSpawned = 0;
    std::uint64_t aliensKilled = 0;
    std::uint64_t humansLost = 0;
    std::uint64_t humansBuilt = 0;
    std::uint64_t shotsFired = 0;
    double ammoDelivered = 0.0;
    double damageDealt = 0.0;
    double damageTaken = 0.0;
};

struct Outcome {
    bool finished = false;
    bool victory = false;
    double time = 0.0;
    int wavesSpawned = 0;
};

class Battlefield {
public:
    explicit Battlefield(Options options);

    void beginTick();
    void endTick();

    [[nodiscard]] gygax::json::Value observe(std::uint32_t id) const;
    [[nodiscard]] gygax::json::Value commanderView() const;
    [[nodiscard]] gygax::json::Value plan(std::uint32_t id, const Vec& target, bool roundTrip) const;
    std::string applyOrders(std::uint32_t id, const gygax::json::Value& orders);
    std::string produce(const std::string& kind);
    void designate(const std::string& sector);
    std::string directive(const std::string& line);
    [[nodiscard]] Directives directives() const;
    [[nodiscard]] std::string status() const;

    [[nodiscard]] const Entity* find(std::uint32_t id) const;
    [[nodiscard]] const Entity* findByName(const std::string& name) const;
    [[nodiscard]] std::vector<std::uint32_t> livingHumanIds() const;
    [[nodiscard]] std::vector<std::uint32_t> newHumanIds();
    [[nodiscard]] std::vector<std::string> takeRetired();
    [[nodiscard]] std::vector<std::string> takeNarrative();
    [[nodiscard]] const std::vector<Entity>& entities() const { return entities_; }
    [[nodiscard]] const Outcome& outcome() const { return outcome_; }
    [[nodiscard]] const Stats& stats() const { return stats_; }
    [[nodiscard]] double time() const { return time_; }
    [[nodiscard]] int tick() const { return tick_; }
    [[nodiscard]] double materiel() const { return materiel_; }
    [[nodiscard]] double hiveRage() const { return rage_; }
    [[nodiscard]] const Options& options() const { return options_; }
    [[nodiscard]] gygax::logistics::Ledger& ledger() { return ledger_; }
    [[nodiscard]] std::uint64_t digest() const;
    [[nodiscard]] std::string render() const;
    [[nodiscard]] int alive(Kind kind) const;
    [[nodiscard]] int aliveAliens() const;
    [[nodiscard]] double bunkerHp() const;

private:
    Entity& spawn(Kind kind, Vec pos, int wave);
    void spawnWave(int wave);
    void substep(double dt);
    void kill(Entity& e, const char* reason);
    void note(std::string line);
    [[nodiscard]] Entity* mutableFind(std::uint32_t id);
    [[nodiscard]] static gygax::logistics::Point toPoint(const Vec& v);

    Options options_;
    gygax::neuro::Rng rng_;
    gygax::logistics::Ledger ledger_;
    std::unique_ptr<gygax::neuro::Network> hive_;
    std::vector<Entity> entities_;
    std::vector<std::uint32_t> newHumans_;
    std::vector<std::string> retired_;
    std::vector<std::string> narrative_;
    std::map<std::string, int> counters_;
    Directives directives_;
    Outcome outcome_;
    Stats stats_;
    double time_ = 0.0;
    double materiel_ = 600.0;
    double rage_ = 1.0;
    int tick_ = 0;
    int nextWave_ = 1;
    std::uint32_t nextId_ = 1;
    std::uint64_t casualtiesThisTick_ = 0;
    mutable std::mutex mutex_;
};

}
