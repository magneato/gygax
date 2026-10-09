#pragma once

// OHFLOCK: a flock of small unarmed aircraft holds an air corridor open for a relief convoy against a
// conventional air defence, and wins without firing anything.
//
// The flock's tools are light, sound and knowledge:
//   - low-power RGB lasers that dazzle *sensors* (missile and gun trackers, designator pods) and paint
//     decoy spots on empty ground; an interlock keeps every beam off cockpits and people;
//   - acoustic projectors that "throw" phantom sound sources for acoustic cueing to chase;
//   - a shared picture, intercept prediction, radar notching, and SatLink: Doppler navigation from a
//     LEO relay constellation while GNSS is jammed, command windows through those relays, and a
//     reconnaissance satellite's pass predicted and waited out.
//
// Everything is fictional and abstract: a 3D kinematic model with probabilistic engagement outcomes,
// meant to be watched and understood, not a weapons-effects simulation. Deterministic for a seed.

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gygax/satlink/satlink.hpp>

namespace ohflock {

struct V3 {
    double x{0}, y{0}, z{0}; // metres: east, north, up (above mean ground)
};
[[nodiscard]] inline V3 operator+(V3 a, V3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] inline V3 operator-(V3 a, V3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] inline V3 operator*(V3 a, double s) {
    return {a.x * s, a.y * s, a.z * s};
}
[[nodiscard]] double norm(V3 a);
[[nodiscard]] V3 unit(V3 a);

enum class Kind { Kite, Fighter, Striker, Sam, Aaa, AcousticArray, Jammer, Truck };
enum class Weapon { IrMissile, RadarMissile, SamRadar, SamOptical, GunBurst, AaaBurst, LaserGuidedBomb };
// How a shot ended. Every OPFOR shot gets exactly one; the flock never shoots.
enum class Outcome { InFlight, Hit, Notched, Outmaneuvered, Dazzled, Phantom, DecoySpot, LostLock, Spent };

[[nodiscard]] const char* name(Kind k);
[[nodiscard]] const char* name(Weapon w);
[[nodiscard]] const char* name(Outcome o);

struct Unit {
    int id{0};
    Kind kind{Kind::Kite};
    std::string call;  // e.g. "R3" (a kite of the red section), "VIPER 1"
    char section{' '}; // kites: R, G, B (laser colour) or E (echo: acoustic section)
    V3 pos, vel;
    bool alive{true};
    bool gone{false}; // OPFOR aircraft that have returned to base
    int missiles{0}, missiles2{0}, rounds{0};
    double fuel{0}; // seconds of flight left
    double laserHeat{0};
    double nextShot{0};
    std::string task; // what the flock brain has it doing, for the viewer
    int dazzledUntilTick{-1};
    double notchSince{-1}; // kites: beaming a radar since this time
    bool breaking{false};  // kites: last-moment break turn against an IR missile
    V3 slot;               // kites: station in the screen; OPFOR: where it is heading
};

struct Shot {
    int id{0};
    Weapon weapon{Weapon::GunBurst};
    int shooter{0}, target{0};
    V3 pos, vel, aim;
    double launched{0}, impactAt{0};
    Outcome outcome{Outcome::InFlight};
    bool againstConvoy{false};
};

struct Beam { // a laser in use this tick
    int from{0};
    V3 to;
    char colour{'R'};
    std::string purpose; // "dazzle SAM tracker", "decoy spot"
};

struct Phantom { // a thrown sound source
    V3 pos;
    int owner{0};
    int array{0}; // the acoustic array it is meant for
};

struct Event {
    double t{0};
    std::string kind; // "brain", "opfor", "shot", "sky", "order", "outcome"
    std::string text;
};

struct SkyTrack { // one satellite as seen from the corridor
    std::string name;
    bool relay{false};
    double elevation{0}, azimuth{0}, doppler_hz{0};
};

struct Stats {
    int opforShots{0}, flockShots{0}, hits{0}, convoyHits{0}, kitesLost{0};
    int byOutcome[9]{};
    double worstNavErrorM{0}, navErrorM{0};
    int ordersApplied{0}, ordersQueued{0};
    int lasersOnSensors{0}, phantomsThrown{0};
    bool convoyThrough{false}, opforBrokeOff{false};
};

struct Config {
    std::uint64_t seed{1984};
    bool satlink{true};
    double dt{0.25};
    double maxSeconds{1200};
};

class Sim {
public:
    explicit Sim(Config config);

    void step();
    [[nodiscard]] bool done() const;
    [[nodiscard]] double time() const { return t_; }
    [[nodiscard]] double startUnix() const { return startUnix_; }
    [[nodiscard]] long tick() const { return tick_; }

    // A human directive (from gyde, --script or the console). Orders travel to the flock through a
    // relay satellite, so with SatLink on they wait for one to be above 25 degrees.
    std::string order(const std::string& directive);
    [[nodiscard]] std::string status() const;
    [[nodiscard]] std::string explain(std::size_t last = 8) const;
    [[nodiscard]] std::string sky() const;

    [[nodiscard]] const std::vector<Unit>& units() const { return units_; }
    [[nodiscard]] const std::vector<Shot>& shots() const { return shots_; }
    [[nodiscard]] const std::vector<Beam>& beams() const { return beams_; }
    [[nodiscard]] const std::vector<Phantom>& phantoms() const { return phantoms_; }
    [[nodiscard]] const std::vector<Event>& events() const { return events_; }
    [[nodiscard]] const std::vector<SkyTrack>& skyTracks() const { return skyTracks_; }
    [[nodiscard]] const Stats& stats() const { return stats_; }
    [[nodiscard]] const Config& config() const { return config_; }
    [[nodiscard]] bool gnssJammed() const { return t_ >= kJamAt; }
    [[nodiscard]] bool reconOverhead() const { return reconOverhead_; }
    [[nodiscard]] bool dispersed() const { return dispersed_; }
    [[nodiscard]] std::string navSource() const { return navSource_; }
    [[nodiscard]] V3 convoyCenter() const;
    [[nodiscard]] std::uint64_t digest() const;

    static constexpr double kOriginLat = 20.40, kOriginLon = 49.10, kGroundM = 310.0;
    static constexpr double kJamAt = 75.0;
    static constexpr double kRelayMaskDeg = 20.0;
    static constexpr double kBeaconHz = 2.2e9;

private:
    struct Rng {
        std::uint64_t s;
        double uniform();
        double normal();
    };

    void setupSky();
    void setupForces();
    void updateSky();
    void navigation();
    void opfor();
    void brain();
    void moveUnits();
    void resolveShots();
    void applyQueuedOrders();
    void log(const std::string& kind, std::string text);
    void fire(Unit& shooter, Weapon w, int target, bool convoy);
    [[nodiscard]] std::optional<int> nearestKite(V3 p, double range) const;
    [[nodiscard]] gygax::satlink::Observer geodetic(V3 p) const;
    [[nodiscard]] bool relayInView() const;
    [[nodiscard]] double placementErrorM() const; // how far off thrown sources and decoy spots land

    Config config_;
    Rng rng_;
    double t_{0};
    long tick_{0};
    double startUnix_{0};
    std::vector<Unit> units_;
    std::vector<Shot> shots_;
    std::vector<Beam> beams_;
    std::vector<Phantom> phantoms_;
    std::vector<Event> events_;
    std::vector<SkyTrack> skyTracks_;
    std::vector<gygax::satlink::Satellite> relays_;
    std::unique_ptr<gygax::satlink::Satellite> recon_;
    double reconPeakUnix_{0};
    bool reconOverhead_{false}, dispersed_{false}, reconSawFlock_{false};
    double reconCueUntil_{-1};
    Stats stats_;
    // Navigation: the flock's belief about where it is, against the truth.
    V3 insDrift_, insDriftRate_;
    struct DopplerRow {
        double unix;
        V3 believed;            // where the inertial units said the flock was
        std::vector<double> hz; // measured beacon Doppler per relay; NaN when below 10 degrees
    };
    std::deque<DopplerRow> dopplerLog_;
    double lastFix_{-1}, lastLoggedFix_{-1};
    double navPc_[2]{25, 25};   // m^2: the flock's variance of its own position error, per axis
    double navPv_[2]{1.0, 1.0}; // (m/s)^2: and of the inertial velocity bias it has not yet removed
    V3 velocityFix_;            // the velocity bias estimated so far, compensated continuously
    [[nodiscard]] int relaysHeard() const;
    std::string navSource_{"GNSS"};
    // Human direction.
    std::deque<std::string> queued_;
    std::string posture_{"screen"}; // screen | tight | wide
    bool lasers_{true}, acoustics_{true};
    double corridorShiftM_{0};
    std::vector<std::string> decisions_;
};

} // namespace ohflock
