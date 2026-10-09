#include "sim.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <sstream>

namespace ohflock {

namespace sat = gygax::satlink;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kEarthRadiusM = 6371000.0;

// Scenario geometry (metres, local east/north/up around the corridor's centre).
constexpr double kRoadStartY = -5200, kRoadEndY = 5200, kConvoySpeed = 13.0;
constexpr int kTrucks = 6;
constexpr double kTruckSpacing = 70.0;
constexpr double kKiteSpeed = 38.0, kKiteDashSpeed = 46.0;
constexpr double kLaserRangeM = 4500.0, kLaserHeatMax = 8.0, kLaserCoolPerS = 0.45;
constexpr double kThrowRangeM = 950.0, kArrayHearingM = 3500.0;
constexpr double kReconMaskDeg = 30.0;
constexpr double kDopplerNoiseHz = 1.0; // a 2.2 GHz beacon over 1 s with a stable reference oscillator

double clampd(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}

// Gauss-Jordan with partial pivoting on a symmetric positive-definite 4x4 system.
void invert4(const double (&a)[4][4], double (&out)[4][4]) {
    double m[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) m[i][j] = j < 4 ? a[i][j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::abs(m[r][c]) > std::abs(m[pivot][c])) pivot = r;
        for (int j = 0; j < 8; ++j) std::swap(m[c][j], m[pivot][j]);
        const double d = m[c][c];
        for (int j = 0; j < 8; ++j) m[c][j] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = m[r][c];
            for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i][j] = m[i][j + 4];
}

void solve4(const double (&a)[4][4], const double (&b)[4], double (&x)[4]) {
    double inv[4][4];
    invert4(a, inv);
    for (int i = 0; i < 4; ++i) {
        x[i] = 0;
        for (int j = 0; j < 4; ++j) x[i] += inv[i][j] * b[j];
    }
}

// A two-line element set built from mean elements, checksums included.
std::pair<std::string, std::string> makeTle(int catalog, const char* intl, int yy, double doy, double incl, double raan, double ecc,
                                            double argp, double mean_anomaly, double mean_motion) {
    auto line1 = std::format("1 {:05}U {:<8} {:02}{:012.8f}  .00001000  00000-0  10000-3 0  999", catalog, intl, yy, doy);
    auto line2 = std::format("2 {:05} {:8.4f} {:8.4f} {:07} {:8.4f} {:8.4f} {:11.8f}{:5}", catalog, incl, raan,
                             static_cast<long>(std::lround(ecc * 1e7)), argp, mean_anomaly, mean_motion, 1000);
    line1 += static_cast<char>('0' + sat::tleChecksum(line1));
    line2 += static_cast<char>('0' + sat::tleChecksum(line2));
    return {line1, line2};
}

} // namespace

double norm(V3 a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
V3 unit(V3 a) {
    const double n = norm(a);
    return n > 1e-9 ? a * (1.0 / n) : V3{1, 0, 0};
}

const char* name(Kind k) {
    switch (k) {
    case Kind::Kite: return "kite";
    case Kind::Fighter: return "fighter";
    case Kind::Striker: return "strike jet";
    case Kind::Sam: return "SAM battery";
    case Kind::Aaa: return "AAA gun";
    case Kind::AcousticArray: return "acoustic array";
    case Kind::Jammer: return "GNSS jammer";
    case Kind::Truck: return "truck";
    }
    return "?";
}
const char* name(Weapon w) {
    switch (w) {
    case Weapon::IrMissile: return "IR missile";
    case Weapon::RadarMissile: return "radar missile";
    case Weapon::SamRadar: return "SAM (radar)";
    case Weapon::SamOptical: return "SAM (optical)";
    case Weapon::GunBurst: return "cannon burst";
    case Weapon::AaaBurst: return "AAA burst";
    case Weapon::LaserGuidedBomb: return "laser-guided bomb";
    }
    return "?";
}
const char* name(Outcome o) {
    switch (o) {
    case Outcome::InFlight: return "in flight";
    case Outcome::Hit: return "hit";
    case Outcome::Notched: return "notched (radar lost a target flying across its beam)";
    case Outcome::Outmaneuvered: return "outmanoeuvred (predicted break turn)";
    case Outcome::Dazzled: return "dazzled (tracker blinded by a laser)";
    case Outcome::Phantom: return "phantom (fired at a thrown sound)";
    case Outcome::DecoySpot: return "decoy spot (bomb followed a painted spot to empty sand)";
    case Outcome::LostLock: return "lost lock (designator dazzled)";
    case Outcome::Spent: return "spent";
    }
    return "?";
}

double Sim::Rng::uniform() {
    s += 0x9E3779B97F4A7C15ULL; // splitmix64: the same numbers on every platform
    std::uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) * 0x1.0p-53;
}
double Sim::Rng::normal() {
    const double u1 = std::max(uniform(), 1e-300), u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
}

Sim::Sim(Config config) : config_(config), rng_{config.seed} {
    setupSky();
    setupForces();
    updateSky();
    brain(); // the flock is on station when the story starts, picket included
    for (auto& u : units_)
        if (u.kind == Kind::Kite) u.pos = u.slot;
    beams_.clear();
    log("brief", "Relief convoy of six trucks enters the Sahran corridor; twenty-four kites screen it. The flock carries no weapons: "
                 "low-power RGB lasers for sensors only (cockpit interlock on), acoustic projectors, and SatLink.");
}

// --- Sky: a relay constellation and the other side's reconnaissance satellite ------------------

void Sim::setupSky() {
    // Fictional satellites on realistic orbits, epoch 2026-10-09 05:00 UTC (day 282.2083).
    constexpr int yy = 26;
    constexpr double doy = 282.20833333;
    // SAHAB relays: a 60-satellite shell at 550 km and 53 degrees, six planes of ten (Walker 60/6/1),
    // so a relay is overhead some of the time and not all of it, as with real small constellations.
    for (int plane = 0; plane < 6; ++plane) {
        for (int slot = 0; slot < 10; ++slot) {
            const int i = plane * 10 + slot;
            const double raan = 15.0 + 60.0 * plane;
            const double m = std::fmod(36.0 * slot + 6.0 * plane, 360.0);
            auto [l1, l2] = makeTle(90101 + i, "26900A", yy, doy, 53.0, raan, 0.0001, 90.0, m, 15.06);
            relays_.emplace_back(std::format("SAHAB-{:02}", i + 1), l1, l2);
        }
    }
    // OKULAR-2: the opposing side's sun-synchronous imager.
    auto [r1, r2] = makeTle(90201, "26901A", yy, doy, 97.45, 147.0, 0.0012, 80.0, 333.0, 15.21);
    recon_ = std::make_unique<sat::Satellite>("OKULAR-2", r1, r2);

    // The battle is timed by the other side to follow their imaging pass: it starts five minutes before
    // the first pass over the corridor that climbs above 45 degrees.
    const sat::Observer here{kOriginLat, kOriginLon, kGroundM};
    const double epoch = recon_->tle().epochUnixSeconds();
    const auto passes = recon_->passes(here, epoch, epoch + 2 * 86400.0, kReconMaskDeg);
    for (const auto& p : passes) {
        if (p.max_elevation_deg >= 45.0) {
            reconPeakUnix_ = p.culmination_unix;
            break;
        }
    }
    if (reconPeakUnix_ == 0.0 && !passes.empty()) reconPeakUnix_ = passes.front().culmination_unix;
    startUnix_ = std::floor(reconPeakUnix_ - 300.0);
}

sat::Observer Sim::geodetic(V3 p) const {
    const double lat = kOriginLat + p.y / kEarthRadiusM * 180.0 / kPi;
    const double lon = kOriginLon + p.x / (kEarthRadiusM * std::cos(kOriginLat * kPi / 180.0)) * 180.0 / kPi;
    return {lat, lon, kGroundM + p.z};
}

void Sim::updateSky() {
    const double now = startUnix_ + t_;
    const auto here = geodetic(convoyCenter());
    skyTracks_.clear();
    for (const auto& r : relays_) {
        const auto look = r.observe(here, now);
        skyTracks_.push_back({r.name(), true, look.elevation_deg, look.azimuth_deg, sat::dopplerShiftHz(kBeaconHz, look.range_rate_km_s)});
    }
    const auto look = recon_->observe(here, now);
    skyTracks_.push_back({recon_->name(), false, look.elevation_deg, look.azimuth_deg, 0.0});
    const bool overhead = look.elevation_deg >= kReconMaskDeg;
    if (overhead && !reconOverhead_)
        log("sky", std::format("{} is overhead ({:.0f} degrees and climbing): its camera covers the corridor", recon_->name(),
                               look.elevation_deg));
    if (!overhead && reconOverhead_) log("sky", std::format("{} has passed", recon_->name()));
    reconOverhead_ = overhead;
}

int Sim::relaysHeard() const {
    int n = 0;
    for (std::size_t r = 0; r < relays_.size(); ++r)
        n += std::any_of(dopplerLog_.begin(), dopplerLog_.end(), [&](const DopplerRow& row) { return !std::isnan(row.hz[r]); });
    return n;
}

bool Sim::relayInView() const {
    return std::any_of(skyTracks_.begin(), skyTracks_.end(), [](const SkyTrack& s) { return s.relay && s.elevation >= kRelayMaskDeg; });
}

// --- Forces ---------------------------------------------------------------------------------------

void Sim::setupForces() {
    int id = 0;
    auto add = [&](Kind k, std::string call, V3 pos) -> Unit& {
        Unit u;
        u.id = ++id;
        u.kind = k;
        u.call = std::move(call);
        u.pos = pos;
        units_.push_back(u);
        return units_.back();
    };
    const char sections[] = {'R', 'G', 'B', 'E'};
    for (int s = 0; s < 4; ++s) {
        for (int i = 0; i < 6; ++i) {
            auto& k = add(Kind::Kite, std::format("{}{}", sections[s], i + 1), {0, kRoadStartY - 1500, 600});
            k.section = sections[s];
        }
    }
    for (int i = 0; i < kTrucks; ++i) add(Kind::Truck, std::format("TRUCK {}", i + 1), {0, kRoadStartY - kTruckSpacing * i, 0});
    // Fighters arrive in pairs; strike jets come for the convoy.
    for (int i = 0; i < 4; ++i) {
        auto& f = add(Kind::Fighter, std::format("VIPER {}", i + 1),
                      {i < 2 ? 1500.0 * i : -42000.0, i < 2 ? 46000.0 : 18000.0 + 1500.0 * i, 7200});
        f.missiles = 4;  // IR
        f.missiles2 = 4; // radar
        f.rounds = 18;   // cannon bursts
        f.fuel = 620;
        f.nextShot = i < 2 ? 40 : 210; // when it enters
        f.gone = true;                 // not on the scene yet
    }
    for (int i = 0; i < 2; ++i) {
        auto& s = add(Kind::Striker, std::format("HAMMER {}", i + 1), {48000.0, -2000.0 + 3000.0 * i, 5200});
        s.missiles = 4; // laser-guided bombs
        s.fuel = 400;
        s.nextShot = i == 0 ? 250 : 470;
        s.gone = true;
    }
    auto& samSite = add(Kind::Sam, "SAM", {9000, 6000, 0});
    samSite.missiles = 10;
    const V3 guns[] = {{-2600, -900, 0}, {2700, 2400, 0}, {-2300, 4700, 0}};
    for (int i = 0; i < 3; ++i) {
        auto& g = add(Kind::Aaa, std::format("GUN {}", i + 1), guns[i]);
        g.rounds = 70;
        add(Kind::AcousticArray, std::format("EAR {}", i + 1), guns[i] + V3{150, -120, 0});
    }
    add(Kind::Jammer, "JAMMER", {-15000, -8000, 0});
}

V3 Sim::convoyCenter() const {
    V3 c{};
    int n = 0;
    for (const auto& u : units_) {
        if (u.kind == Kind::Truck) {
            c = c + u.pos;
            ++n;
        }
    }
    return n ? c * (1.0 / n) : V3{};
}

void Sim::log(const std::string& kind, std::string text) {
    events_.push_back({t_, kind, std::move(text)});
    if (kind == "brain") {
        decisions_.push_back(std::format("T+{:03.0f}s {}", t_, events_.back().text));
        if (decisions_.size() > 200) decisions_.erase(decisions_.begin());
    }
}

std::optional<int> Sim::nearestKite(V3 p, double range) const {
    std::optional<int> best;
    double bestD = range;
    for (const auto& u : units_) {
        if (u.kind != Kind::Kite || !u.alive) continue;
        const double d = norm(u.pos - p);
        if (d < bestD) {
            bestD = d;
            best = u.id;
        }
    }
    return best;
}

// --- Navigation: GNSS, then LEO Doppler under jamming ----------------------------------------------

double Sim::placementErrorM() const {
    return stats_.navErrorM;
}

void Sim::navigation() {
    if (!gnssJammed()) {
        insDrift_ = {rng_.normal() * 3.0, rng_.normal() * 3.0, 0};
        navSource_ = "GNSS";
    } else {
        if (navSource_ == "GNSS") {
            // The inertial units start drifting the moment GNSS goes.
            const double a = rng_.uniform() * 2.0 * kPi;
            insDriftRate_ = {std::cos(a) * 0.9, std::sin(a) * 0.9, 0};
            navSource_ = config_.satlink ? "inertial (awaiting LEO Doppler fix)" : "inertial only";
            log("opfor", "GNSS jamming starts across the corridor; every receiver in the flock loses lock");
            log("brain", config_.satlink ? "GNSS lost: navigating inertially and listening to SAHAB relay beacons for a Doppler fix"
                                         : "GNSS lost: navigating inertially (no satellite tracking aboard)");
        }
        insDrift_ = insDrift_ + (insDriftRate_ - velocityFix_) * config_.dt;
        if (config_.satlink && tick_ % 2 == 0) {
            // Twice a second, record each relay's beacon Doppler as received at the flock's true position, with noise.
            const double now = startUnix_ + t_;
            const auto truth = geodetic(convoyCenter() + V3{0, 0, 600});
            std::vector<double> row;
            for (const auto& r : relays_) {
                const auto look = r.observe(truth, now);
                row.push_back(look.elevation_deg >= 10.0
                                  ? sat::dopplerShiftHz(kBeaconHz, look.range_rate_km_s) + rng_.normal() * kDopplerNoiseHz
                                  : std::numeric_limits<double>::quiet_NaN());
            }
            dopplerLog_.push_back({now, convoyCenter() + V3{0, 0, 600} + insDrift_, std::move(row)});
            while (!dopplerLog_.empty() && dopplerLog_.front().unix < now - 60.0) dopplerLog_.pop_front();
        }
        if (config_.satlink && t_ - lastFix_ >= 60.0) {
            // Least squares on the inertial error: which offset, applied to the track the inertial units
            // believe the flock flew, best explains the Doppler curves heard since the last fix?
            // (Transit's idea, with SGP4 predicting every relay's motion.)
            std::vector<std::pair<std::size_t, std::size_t>> obs; // (row, relay)
            for (std::size_t i = 0; i < dopplerLog_.size(); ++i)
                for (std::size_t r = 0; r < relays_.size(); ++r)
                    if (!std::isnan(dopplerLog_[i].hz[r])) obs.emplace_back(i, r);
            if (obs.size() >= 100) {
                const V3 truth = convoyCenter() + V3{0, 0, 600};
                // Maximum a posteriori over four unknowns: the inertial position error now (cx, cy) and the
                // residual velocity bias (vx, vy) still in the inertial track. Row i is corrected by
                // c + v (t_i - now). Priors: the flock's covariance of each, so a direction one relay cannot
                // see falls back on what the flock already knew, and the posterior says honestly what was learned.
                const double now = startUnix_ + t_;
                const double since = t_ - std::max(lastFix_, kJamAt);
                const double pc[2] = {navPc_[0] + navPv_[0] * since * since + 25.0, navPc_[1] + navPv_[1] * since * since + 25.0};
                const double pv[2] = {navPv_[0] + 0.004 * since, navPv_[1] + 0.004 * since};  // the bias itself wanders a little
                const double prior[4] = {1.0 / pc[0], 1.0 / pc[1], 1.0 / pv[0], 1.0 / pv[1]}; // diagonal information
                const double noise2 = kDopplerNoiseHz * kDopplerNoiseHz;
                double theta[4] = {0, 0, 0, 0};
                auto offset = [&](std::size_t i, const double* th) {
                    const double dt = dopplerLog_[i].unix - now;
                    return V3{th[0] + th[2] * dt, th[1] + th[3] * dt, 0};
                };
                auto residuals = [&](const double* th) {
                    std::vector<double> res;
                    for (const auto& [i, r] : obs) {
                        const auto look = relays_[r].observe(geodetic(dopplerLog_[i].believed + offset(i, th)), dopplerLog_[i].unix);
                        res.push_back(dopplerLog_[i].hz[r] - sat::dopplerShiftHz(kBeaconHz, look.range_rate_km_s));
                    }
                    return res;
                };
                double h[4][4] = {};
                for (int iter = 0; iter < 6; ++iter) {
                    const auto r0 = residuals(theta);
                    std::vector<double> jac[4];
                    const double probe[4] = {20.0, 20.0, 0.2, 0.2};
                    for (int p = 0; p < 4; ++p) {
                        double th[4] = {theta[0], theta[1], theta[2], theta[3]};
                        th[p] += probe[p];
                        const auto rp = residuals(th);
                        jac[p].resize(r0.size());
                        for (std::size_t k = 0; k < r0.size(); ++k) jac[p][k] = (rp[k] - r0[k]) / probe[p];
                    }
                    // Gauss-Newton on |r|^2/sigma^2 + theta^T Prior theta: H step = -(J^T r / sigma^2 + Prior theta).
                    double g[4];
                    for (int p = 0; p < 4; ++p) {
                        g[p] = prior[p] * theta[p];
                        for (std::size_t k = 0; k < r0.size(); ++k) g[p] += jac[p][k] * r0[k] / noise2;
                        for (int q = 0; q < 4; ++q) {
                            h[p][q] = p == q ? prior[p] : 0.0;
                            for (std::size_t k = 0; k < r0.size(); ++k) h[p][q] += jac[p][k] * jac[q][k] / noise2;
                        }
                    }
                    double stepv[4];
                    solve4(h, g, stepv);
                    double size = 0;
                    for (int p = 0; p < 4; ++p) theta[p] -= stepv[p], size += stepv[p] * stepv[p];
                    if (std::sqrt(size) < 0.5) break;
                }
                double cov[4][4];
                invert4(h, cov);
                navPc_[0] = cov[0][0], navPc_[1] = cov[1][1], navPv_[0] = std::max(cov[2][2], 1e-4), navPv_[1] = std::max(cov[3][3], 1e-4);
                const V3 correction{theta[0], theta[1], 0};
                // theta[2..3] corrects the track, so it is minus the residual bias: remove that bias from here on.
                velocityFix_ = velocityFix_ - V3{theta[2], theta[3], 0};
                const double fixSigma = std::sqrt(navPc_[0] + navPc_[1]), priorSigma = std::sqrt(pc[0] + pc[1]);
                const double before = norm(insDrift_);
                const V3 belief = truth + insDrift_ + correction;
                insDrift_ = belief - truth;
                // Each measurement informs exactly one fix: what it taught now lives in the covariance.
                const int heard = relaysHeard();
                dopplerLog_.clear();
                lastFix_ = t_;
                navSource_ = "LEO Doppler (SAHAB)";
                if (lastLoggedFix_ < 0 || t_ - lastLoggedFix_ >= 90.0) {
                    log("brain",
                        std::format("SatLink Doppler fix: {} beacon measurements from {} relay{}; uncertainty {:.0f} m -> {:.0f} m, "
                                    "true error {:.0f} m -> {:.0f} m",
                                    obs.size(), heard, heard == 1 ? "" : "s", priorSigma, fixSigma, before, norm(insDrift_)));
                    lastLoggedFix_ = t_;
                }
            }
        }
    }
    stats_.navErrorM = norm(insDrift_);
    stats_.worstNavErrorM = std::max(stats_.worstNavErrorM, stats_.navErrorM);
}

// --- The other side ---------------------------------------------------------------------------------

void Sim::fire(Unit& shooter, Weapon w, int target, bool convoy) {
    const Unit* tgt = nullptr;
    for (const auto& u : units_)
        if (u.id == target) tgt = &u;
    if (tgt == nullptr) return;
    Shot s;
    s.id = static_cast<int>(shots_.size()) + 1;
    s.weapon = w;
    s.shooter = shooter.id;
    s.target = target;
    s.pos = shooter.pos;
    s.aim = tgt->pos;
    s.launched = t_;
    s.againstConvoy = convoy;
    double speed = 800;
    switch (w) {
    case Weapon::IrMissile: speed = 700; break;
    case Weapon::RadarMissile: speed = 950; break;
    case Weapon::SamRadar:
    case Weapon::SamOptical: speed = 780; break;
    case Weapon::GunBurst: speed = 1000; break;
    case Weapon::AaaBurst: speed = 950; break;
    case Weapon::LaserGuidedBomb: speed = 260; break;
    }
    s.impactAt = t_ + std::max(1.0, norm(s.aim - s.pos) / speed);
    s.vel = unit(s.aim - s.pos) * speed;
    shots_.push_back(s);
    ++stats_.opforShots;
}

void Sim::opfor() {
    const V3 convoy = convoyCenter();
    V3 flock{};
    int nk = 0;
    for (const auto& u : units_)
        if (u.kind == Kind::Kite && u.alive) flock = flock + u.pos, ++nk;
    if (nk) flock = flock * (1.0 / nk);
    const bool recon = t_ < reconCueUntil_; // the imaging pass located the flock: better cueing

    for (auto& u : units_) {
        if (u.kind == Kind::Fighter || u.kind == Kind::Striker) {
            if (u.gone && u.fuel > 0 && t_ >= u.nextShot && u.vel.x == 0 && u.vel.y == 0) {
                u.gone = false; // enters the fight
                u.vel = unit(flock - u.pos) * 240.0;
                log("opfor",
                    std::format("{} enters from the {} at {:.0f} m", u.call,
                                std::abs(u.pos.y) > std::abs(u.pos.x) ? (u.pos.y > 0 ? "north" : "south") : (u.pos.x > 0 ? "east" : "west"),
                                u.pos.z));
            }
            if (u.gone) continue;
            u.fuel -= config_.dt;
            const bool dry = u.kind == Kind::Fighter ? (u.missiles + u.missiles2 + u.rounds == 0) : u.missiles == 0;
            if (dry || u.fuel <= 0) {
                if (u.task != "rtb") {
                    u.task = "rtb";
                    log("opfor", std::format("{} {} and turns for home", u.call, dry ? "has nothing left to fire" : "is at bingo fuel"));
                }
                u.slot = u.pos + unit(V3{u.pos.x, u.pos.y, 0}) * 60000.0;
                if (norm(V3{u.pos.x, u.pos.y, 0}) > 45000) u.gone = true;
                continue;
            }
        }
        switch (u.kind) {
        case Kind::Fighter: {
            const double d = norm(flock - u.pos);
            const bool guns = u.missiles + u.missiles2 == 0;
            // Stand off with missiles; close in for cannon passes when they are gone.
            const V3 off = unit(V3{u.pos.x - flock.x, u.pos.y - flock.y, 0}) * (guns ? 900.0 : 9000.0);
            u.slot = flock + off + V3{0, 0, guns ? 1300.0 : 6500.0};
            if (t_ < u.nextShot) break;
            const auto target = nearestKite(u.pos, guns ? 1600.0 : 26000.0);
            if (!target) break;
            if (!guns && u.missiles2 > 0 && d > 9000) {
                --u.missiles2;
                fire(u, Weapon::RadarMissile, *target, false);
                u.nextShot = t_ + 9.0;
            } else if (!guns && u.missiles > 0 && d <= 9000) {
                if (rng_.uniform() < 0.45) { // a kite's heat signature is tiny: most lock attempts fail
                    --u.missiles;
                    fire(u, Weapon::IrMissile, *target, false);
                }
                u.nextShot = t_ + 5.0;
            } else if (!guns && u.missiles2 > 0) {
                --u.missiles2;
                fire(u, Weapon::RadarMissile, *target, false);
                u.nextShot = t_ + 9.0;
            } else if (guns && u.rounds > 0) {
                --u.rounds;
                fire(u, Weapon::GunBurst, *target, false);
                u.nextShot = t_ + 3.0;
            }
            break;
        }
        case Kind::Striker: {
            u.slot = convoy + unit(V3{u.pos.x - convoy.x, u.pos.y - convoy.y, 0}) * 6500.0 + V3{0, 0, 5200};
            if (norm(convoy - u.pos) < 9000 && t_ >= u.nextShot && u.missiles > 0) {
                int truck = 0;
                for (const auto& v : units_)
                    if (v.kind == Kind::Truck && (truck == 0 || rng_.uniform() < 0.3)) truck = v.id;
                --u.missiles;
                fire(u, Weapon::LaserGuidedBomb, truck, true);
                log("opfor", std::format("{} lases the convoy and releases a laser-guided bomb", u.call));
                u.nextShot = t_ + 7.0;
            }
            break;
        }
        case Kind::Sam: {
            if (u.missiles <= 0 || t_ < std::max(30.0, u.nextShot)) break;
            const auto target = nearestKite(u.pos, 22000.0);
            if (!target) break;
            --u.missiles;
            const bool optical = rng_.uniform() < 0.45;
            fire(u, optical ? Weapon::SamOptical : Weapon::SamRadar, *target, false);
            u.nextShot = t_ + 14.0;
            if (u.missiles == 0) log("opfor", "SAM battery has fired its last missile");
            break;
        }
        case Kind::Aaa: {
            if (u.rounds <= 0 || t_ < u.nextShot) break;
            // Cue: the array next to the gun says where the loudest aircraft is; sometimes the gunner's sight.
            const Unit* ear = nullptr;
            for (const auto& v : units_)
                if (v.kind == Kind::AcousticArray && norm(v.pos - u.pos) < 400) ear = &v;
            const bool sight = rng_.uniform() < 0.3;
            std::optional<int> real = nearestKite(u.pos, 2600.0);
            if (!real) break;
            const int targetId = *real;
            bool phantom = false;
            if (sight) {
                if (u.dazzledUntilTick >= tick_) {
                    fire(u, Weapon::AaaBurst, targetId, false);
                    shots_.back().outcome = Outcome::Dazzled; // the gunner fires blind into the glare
                    shots_.back().aim = shots_.back().aim + V3{rng_.normal() * 300, rng_.normal() * 300, rng_.normal() * 150};
                    ++stats_.byOutcome[static_cast<int>(Outcome::Dazzled)];
                    u.nextShot = t_ + 2.5;
                    --u.rounds;
                    break;
                }
            } else if (ear != nullptr) {
                // Loudness ~ 1/d^2; a thrown source is pushed louder than a kite's engine.
                V3 kitePos{};
                for (const auto& k : units_)
                    if (k.id == targetId) kitePos = k.pos;
                double loudest = 1.0 / std::max(1.0, std::pow(norm(kitePos - ear->pos), 2));
                for (const auto& ph : phantoms_) {
                    if (ph.array != ear->id) continue;
                    const double misplaced = placementErrorM();
                    const double effective =
                        2.6 / std::max(1.0, std::pow(norm(ph.pos - ear->pos) + misplaced, 2)) * (misplaced > 250 ? 250.0 / misplaced : 1.0);
                    if (effective > loudest && (!recon || rng_.uniform() >= 0.5)) {
                        loudest = effective;
                        phantom = true;
                    }
                }
            }
            fire(u, Weapon::AaaBurst, targetId, false);
            if (phantom) {
                auto& s = shots_.back();
                s.outcome = Outcome::Phantom;
                for (const auto& ph : phantoms_)
                    if (ph.array == ear->id) s.aim = ph.pos;
                s.vel = unit(s.aim - s.pos) * 950.0;
                ++stats_.byOutcome[static_cast<int>(Outcome::Phantom)];
            }
            --u.rounds;
            u.nextShot = t_ + 2.5;
            if (u.rounds == 0) log("opfor", std::format("{} is out of ammunition", u.call));
            break;
        }
        default: break;
        }
    }
}

// --- The flock's brain ------------------------------------------------------------------------------

void Sim::brain() {
    beams_.clear();
    phantoms_.clear();
    const V3 convoy = convoyCenter();
    const double now = startUnix_ + t_;
    const bool decide = tick_ % 4 == 0;

    // 1. Satellites. With SatLink the flock knows when the other side's camera will be overhead
    //    and spreads out and drops low beforehand; without it, the camera sees a tight flock.
    if (config_.satlink) {
        const auto here = geodetic(convoy);
        const auto in60 = recon_->observe(here, now + 60.0).elevation_deg;
        const bool shouldDisperse = reconOverhead_ || in60 >= kReconMaskDeg;
        if (shouldDisperse && !dispersed_)
            log("brain", std::format("SatLink: {} rises above {:.0f} degrees in under a minute; dispersing and descending until it sets",
                                     recon_->name(), kReconMaskDeg));
        if (!shouldDisperse && dispersed_) log("brain", "SatLink: reconnaissance pass over; re-forming the screen");
        dispersed_ = shouldDisperse;
    }
    if (reconOverhead_ && !dispersed_ && !reconSawFlock_) {
        reconSawFlock_ = true;
        reconCueUntil_ = t_ + 240.0;
        log("opfor",
            std::format("{} images a tight formation over the convoy; ground fire gets better cues for four minutes", recon_->name()));
    }

    // 2. Stations: a screen ahead, on the flanks and behind, scaled by posture and dispersal.
    const double scale = (posture_ == "tight" ? 0.65 : posture_ == "wide" ? 1.45 : 1.0) * (dispersed_ ? 2.0 : 1.0);
    const double alt = dispersed_ ? 160.0 : 650.0;
    const V3 centre = convoy + V3{corridorShiftM_, 0, 0};
    int index = 0;
    for (auto& k : units_) {
        if (k.kind != Kind::Kite || !k.alive) continue;
        const int s = index / 6, i = index % 6;
        ++index;
        const double spread = (i - 2.5) * 420.0 * scale;
        V3 slot;
        switch (s) {
        case 0: slot = {spread, 2600.0 * scale, alt + 120.0 * (i % 2)}; break;          // red: ahead
        case 1: slot = {-2100.0 * scale, spread, alt + 80.0 * (i % 3)}; break;          // green: west flank
        case 2: slot = {2100.0 * scale, spread, alt + 80.0 * (i % 3)}; break;           // blue: east flank
        default: slot = {spread * 0.8, -1700.0 * scale, alt + 250.0 + 60.0 * i}; break; // echo: behind and above
        }
        k.slot = centre + slot;
        k.task = dispersed_ ? "dispersed, low" : "screen";
        k.breaking = false;
        k.laserHeat = std::max(0.0, k.laserHeat - kLaserCoolPerS * config_.dt);
    }

    // A picket: the SAM's position is known, so one kite stands off it inside laser range of its
    // tracker (and outside the guns), ready before the first launch rather than after it.
    for (const auto& sam : units_) {
        if (sam.kind != Kind::Sam || sam.missiles <= 0) continue;
        for (auto& k : units_) {
            if (k.kind == Kind::Kite && k.alive && k.section == 'B') {
                k.slot = sam.pos + unit(V3{convoy.x - sam.pos.x, convoy.y - sam.pos.y, 0}) * 3500.0 + V3{0, 0, 700};
                k.task = "picket on the SAM battery";
                break;
            }
        }
    }

    auto kite = [&](int id) -> Unit* {
        for (auto& u : units_)
            if (u.id == id && u.kind == Kind::Kite && u.alive) return &u;
        return nullptr;
    };

    // 3. Inbound shots: notch radars, break against IR missiles at the predicted moment.
    int notching = 0, breaking = 0;
    for (auto& s : shots_) {
        if (s.outcome != Outcome::InFlight) continue;
        Unit* k = kite(s.target);
        if (k == nullptr) continue;
        const double tgo = s.impactAt - t_;
        if (s.weapon == Weapon::RadarMissile || s.weapon == Weapon::SamRadar) {
            // Fly across the missile's line of sight: zero closing speed hides a slow target in the clutter notch.
            const V3 los = unit(V3{s.pos.x - k->pos.x, s.pos.y - k->pos.y, 0});
            const V3 across{-los.y, los.x, 0};
            k->slot = k->pos + across * 600.0 + V3{0, 0, -40};
            if (k->notchSince < 0) k->notchSince = t_;
            k->task = std::format("notching {}", name(s.weapon));
            ++notching;
        } else if (s.weapon == Weapon::IrMissile && tgo < 2.6) {
            const V3 los = unit(s.pos - k->pos);
            k->slot = k->pos + V3{-los.y, los.x, 0.6} * 300.0;
            k->breaking = true;
            k->task = "break turn";
            ++breaking;
        }
    }
    for (auto& k : units_)
        if (k.kind == Kind::Kite && k.task.rfind("notching", 0) != 0) k.notchSince = -1;

    // 4. Lasers on sensors (never on people: the interlock refuses any line through a cockpit).
    std::vector<int> dazzled;
    auto dazzle = [&](Unit& sensor, const std::string& what, double range) {
        if (!lasers_) return;
        Unit* best = nullptr;
        double bestD = range;
        for (auto& k : units_) {
            if (k.kind != Kind::Kite || !k.alive || k.laserHeat > kLaserHeatMax - 1.0) continue;
            const double d = norm(k.pos - sensor.pos);
            if (d < bestD) bestD = d, best = &k;
        }
        if (best == nullptr) return;
        best->laserHeat += 1.2 * config_.dt;
        beams_.push_back({best->id, sensor.pos + V3{0, 0, sensor.kind == Kind::Striker ? -3.0 : 4.0},
                          best->section == 'E' ? 'W' : best->section, "dazzle " + what});
        sensor.dazzledUntilTick = static_cast<int>(tick_) + 2;
        if (best->task == "screen" || best->task == "dispersed, low") best->task = "dazzling " + what;
        dazzled.push_back(sensor.id);
        ++stats_.lasersOnSensors;
    };
    for (auto& u : units_) {
        if (u.kind == Kind::Sam) {
            const bool opticalInFlight = std::any_of(shots_.begin(), shots_.end(), [&](const Shot& s) {
                return s.outcome == Outcome::InFlight && s.weapon == Weapon::SamOptical;
            });
            if (opticalInFlight || (u.missiles > 0 && nearestKite(u.pos, 9000)))
                dazzle(u, "SAM electro-optical tracker", kLaserRangeM * 2.2);
        } else if (u.kind == Kind::Aaa && u.rounds > 0 && nearestKite(u.pos, 3000)) {
            dazzle(u, std::format("{} sight", u.call), kLaserRangeM);
        } else if (u.kind == Kind::Striker && !u.gone && norm(u.pos - convoy) < 12000) {
            dazzle(u, std::format("{} targeting pod (below the cockpit line)", u.call), kLaserRangeM * 2.6);
        }
    }
    // Decoy spots: a painted spot on empty sand for any bomb still looking for one.
    for (auto& s : shots_) {
        if (s.outcome != Outcome::InFlight || s.weapon != Weapon::LaserGuidedBomb || !lasers_) continue;
        const double side = (s.id % 2) ? 1.0 : -1.0;
        const V3 spot = convoy + V3{side * 650.0, 220.0, 0} + V3{rng_.normal(), rng_.normal(), 0} * placementErrorM();
        if (auto nearest = nearestKite(spot, 7000.0)) {
            Unit* k = kite(*nearest);
            beams_.push_back({k->id, spot, 'G', "decoy spot"});
            s.aim = spot;
            if (k->task == "screen") k->task = "painting a decoy spot";
        }
    }

    // 5. Thrown sound: a phantom near each acoustic array that can hear the flock.
    if (acoustics_) {
        for (const auto& ear : units_) {
            if (ear.kind != Kind::AcousticArray) continue;
            const auto heard = nearestKite(ear.pos, kArrayHearingM);
            if (!heard) continue;
            Unit* thrower = nullptr;
            double bestD = 1e18;
            for (auto& k : units_) {
                if (k.kind != Kind::Kite || !k.alive) continue;
                const double d = norm(k.pos - ear.pos) - (k.section == 'E' ? 1500.0 : 0.0); // echo section first
                if (d < bestD) bestD = d, thrower = &k;
            }
            if (thrower == nullptr) continue;
            // Put the phantom between the thrower and the array, off to one side, in empty sky.
            const V3 toEar = unit(ear.pos - thrower->pos);
            V3 spot = thrower->pos + V3{toEar.x * 0.8 - toEar.y * 0.6, toEar.y * 0.8 + toEar.x * 0.6, 0} * kThrowRangeM;
            spot.z = 420.0 + 30.0 * (ear.id % 3);
            phantoms_.push_back({spot, thrower->id, ear.id});
            if (thrower->task == "screen" || thrower->task == "dispersed, low") thrower->task = "throwing sound at " + ear.call;
            if (tick_ % 8 == 0) ++stats_.phantomsThrown;
        }
    }

    if (decide && (notching || breaking) && tick_ % 120 == 0)
        log("brain",
            std::format("{} kite{} notching radar missiles, {} breaking against IR missiles, {} lasers on sensors, {} phantoms in the air",
                        notching, notching == 1 ? "" : "s", breaking, beams_.size(), phantoms_.size()));
}

// --- Motion and outcomes ----------------------------------------------------------------------------

void Sim::moveUnits() {
    for (auto& u : units_) {
        if (!u.alive || u.gone) continue;
        double speed = 0, agility = 0.6;
        switch (u.kind) {
        case Kind::Kite:
            speed = u.breaking ? kKiteDashSpeed : kKiteSpeed;
            agility = u.breaking ? 4.0 : 1.4;
            break;
        case Kind::Fighter: speed = u.rounds > 0 && u.missiles + u.missiles2 == 0 ? 210.0 : 240.0, agility = 0.35; break;
        case Kind::Striker: speed = 220.0, agility = 0.3; break;
        case Kind::Truck: {
            if (u.pos.y < kRoadEndY + 400) u.pos.y += kConvoySpeed * config_.dt;
            continue;
        }
        default: continue;
        }
        const V3 want = u.slot - u.pos;
        const double dist = norm(want);
        const double v = u.kind == Kind::Kite ? std::min(speed, std::max(6.0, dist * 0.4)) : speed;
        const V3 desired = unit(want) * v;
        const double k = clampd(agility * config_.dt, 0.0, 1.0);
        u.vel = u.vel + (desired - u.vel) * k;
        if (norm(u.vel) > speed) u.vel = unit(u.vel) * speed;
        u.pos = u.pos + u.vel * config_.dt;
        u.pos.z = std::max(u.kind == Kind::Kite ? 60.0 : 300.0, u.pos.z);
    }
}

void Sim::resolveShots() {
    const bool recon = t_ < reconCueUntil_;
    for (auto& s : shots_) {
        if (s.outcome != Outcome::InFlight && s.impactAt < t_ - 3.0) continue; // settled and faded
        Unit* target = nullptr;
        for (auto& u : units_)
            if (u.id == s.target) target = &u;
        if (s.outcome == Outcome::InFlight && target != nullptr && s.weapon != Weapon::LaserGuidedBomb && s.weapon != Weapon::AaaBurst &&
            s.weapon != Weapon::GunBurst)
            s.aim = target->pos; // missiles home
        // Fly toward the aim point so that the shot arrives at impactAt.
        const double left = std::max(config_.dt, s.impactAt - t_);
        if (t_ <= s.impactAt) {
            s.vel = (s.aim - s.pos) * (1.0 / left);
            s.pos = s.pos + s.vel * config_.dt;
        } else {
            s.pos = s.pos + s.vel * config_.dt * 0.3; // spent: falls away
            s.pos.z = std::max(0.0, s.pos.z - 30.0 * config_.dt);
        }
        if (s.outcome == Outcome::Phantom || s.outcome == Outcome::Dazzled) continue; // decided at the gun: never aimed at a kite
        if (s.outcome != Outcome::InFlight || t_ < s.impactAt) continue;

        // Impact: what did the flock do about it?
        Unit* shooter = nullptr;
        for (auto& u : units_)
            if (u.id == s.shooter) shooter = &u;
        Outcome out = Outcome::Hit;
        double p = 0;
        switch (s.weapon) {
        case Weapon::RadarMissile:
        case Weapon::SamRadar:
            if (target && target->notchSince >= 0 && t_ - target->notchSince >= 2.0)
                p = 0.02, out = Outcome::Notched;
            else
                p = 0.3, out = Outcome::Notched;
            break;
        case Weapon::IrMissile: p = target && target->breaking ? 0.03 : 0.3, out = Outcome::Outmaneuvered; break;
        case Weapon::SamOptical: p = shooter && shooter->dazzledUntilTick >= tick_ ? 0.0 : 0.3, out = Outcome::Dazzled; break;
        case Weapon::GunBurst: p = 0.06, out = Outcome::Outmaneuvered; break;
        case Weapon::AaaBurst: p = recon ? 0.14 : 0.05, out = Outcome::Outmaneuvered; break; // a 2 m kite jinking at 600 m
        case Weapon::LaserGuidedBomb: {
            const bool podDazzled = shooter && shooter->dazzledUntilTick >= tick_ - 4 * 8;
            const bool decoy = norm(s.aim - convoyCenter()) > 400.0 && placementErrorM() < 450.0;
            if (decoy)
                p = 0.0, out = Outcome::DecoySpot;
            else if (podDazzled)
                p = 0.05, out = Outcome::LostLock;
            else
                p = 0.8, out = Outcome::LostLock;
            break;
        }
        }
        if (target && target->alive && rng_.uniform() < p) out = Outcome::Hit;
        s.outcome = out;
        ++stats_.byOutcome[static_cast<int>(out)];
        if (out == Outcome::Hit && target) {
            ++stats_.hits;
            if (target->kind == Kind::Truck) {
                ++stats_.convoyHits;
                log("outcome", std::format("{} hit {}", name(s.weapon), target->call));
            } else {
                target->alive = false;
                ++stats_.kitesLost;
                log("outcome", std::format("{} hit kite {}; the flock closes the gap", name(s.weapon), target->call));
            }
        }
    }
}

void Sim::applyQueuedOrders() {
    if (queued_.empty()) return;
    if (!relayInView()) return;
    while (!queued_.empty()) {
        const auto o = queued_.front();
        queued_.pop_front();
        std::istringstream in(o);
        std::string verb, arg;
        in >> verb >> arg;
        if (verb == "posture")
            posture_ = arg;
        else if (verb == "lasers")
            lasers_ = arg == "on";
        else if (verb == "acoustics")
            acoustics_ = arg == "on";
        else if (verb == "corridor")
            corridorShiftM_ = arg == "east" ? 1500.0 : arg == "west" ? -1500.0 : 0.0;
        ++stats_.ordersApplied;
        log("order", std::format("order '{}' reached the flock through the relay", o));
        log("brain", std::format("acknowledged '{}'", o));
    }
}

std::string Sim::order(const std::string& directive) {
    std::istringstream in(directive);
    std::string verb, arg, extra;
    in >> verb >> arg >> extra;
    const bool ok = (verb == "posture" && (arg == "screen" || arg == "tight" || arg == "wide")) ||
                    ((verb == "lasers" || verb == "acoustics") && (arg == "on" || arg == "off")) ||
                    (verb == "corridor" && (arg == "east" || arg == "west" || arg == "centre" || arg == "center"));
    if (!ok || !extra.empty())
        return "unknown order; try: posture screen|tight|wide, lasers on|off, acoustics on|off, corridor east|west|centre";
    const std::string text = verb + " " + arg;
    if (!config_.satlink && !relayInView()) {
        log("order", std::format("order '{}' sent blind with no relay overhead: lost", text));
        return "lost: without satellite tracking the ground station transmitted with no relay overhead";
    }
    queued_.push_back(text);
    ++stats_.ordersQueued;
    if (relayInView()) return "sent through the relay now overhead";
    // SatLink predicts the next relay window.
    const auto here = geodetic(convoyCenter());
    double next = 1e18;
    std::string who;
    for (const auto& r : relays_) {
        const auto p = r.passes(here, startUnix_ + t_, startUnix_ + t_ + 3600.0, kRelayMaskDeg);
        if (!p.empty() && p.front().rise_unix < next) next = p.front().rise_unix, who = r.name();
    }
    return std::format("queued: no relay overhead; {} rises above {:.0f} degrees in {:.0f} s (SatLink)", who, kRelayMaskDeg,
                       next - startUnix_ - t_);
}

void Sim::step() {
    if (tick_ % 4 == 0) updateSky();
    navigation();
    applyQueuedOrders();
    opfor();
    brain();
    moveUnits();
    resolveShots();
    // OPFOR breaks off once nothing of theirs can still act.
    const bool airDone = std::all_of(units_.begin(), units_.end(), [](const Unit& u) {
        return (u.kind != Kind::Fighter && u.kind != Kind::Striker) || (u.gone && u.task == "rtb");
    });
    const bool groundDone = std::all_of(units_.begin(), units_.end(), [](const Unit& u) {
        return (u.kind != Kind::Sam || u.missiles == 0) && (u.kind != Kind::Aaa || u.rounds == 0);
    });
    if (!stats_.convoyThrough && convoyCenter().y >= kRoadEndY) {
        stats_.convoyThrough = true;
        log("outcome",
            std::format("The convoy is through the corridor: {} truck hits, {} of 24 kites lost, {} shots fired at the flock and convoy, "
                        "none fired by it",
                        stats_.convoyHits, stats_.kitesLost, stats_.opforShots));
    }
    if (!stats_.opforBrokeOff && (airDone && t_ > 300) && (groundDone || stats_.convoyThrough)) {
        stats_.opforBrokeOff = true;
        log("outcome", "The other side breaks off: aircraft home, launchers empty");
    }
    t_ += config_.dt;
    ++tick_;
}

bool Sim::done() const {
    const bool settled = std::none_of(shots_.begin(), shots_.end(), [&](const Shot& s) { return s.outcome == Outcome::InFlight; });
    return t_ >= config_.maxSeconds || (stats_.convoyThrough && stats_.opforBrokeOff && settled) ||
           (stats_.convoyThrough && t_ > config_.maxSeconds * 0.9);
}

std::string Sim::status() const {
    int kites = 0, fighters = 0;
    for (const auto& u : units_) {
        kites += u.kind == Kind::Kite && u.alive;
        fighters += (u.kind == Kind::Fighter || u.kind == Kind::Striker) && !u.gone;
    }
    const V3 c = convoyCenter();
    std::string out = std::format("T+{:.0f}s  convoy {:.0f}% through, {} hits  ·  kites {}/24  ·  OPFOR aircraft up {}\n"
                                  "shots at us {} (hits {})  ·  shots by us 0  ·  laser time on sensors {:.0f} s  ·  phantoms thrown {}\n"
                                  "posture {}  ·  lasers {}  ·  acoustics {}  ·  nav {} ({:.0f} m error)  ·  relay {}  ·  orders queued {}",
                                  t_, 100.0 * clampd((c.y - kRoadStartY) / (kRoadEndY - kRoadStartY), 0, 1), stats_.convoyHits, kites,
                                  fighters, stats_.opforShots, stats_.hits, stats_.lasersOnSensors * config_.dt, stats_.phantomsThrown,
                                  posture_, lasers_ ? "on" : "off", acoustics_ ? "on" : "off", navSource_, stats_.navErrorM,
                                  relayInView() ? "in view" : "none overhead", queued_.size());
    return out;
}

std::string Sim::explain(std::size_t last) const {
    std::string out;
    const std::size_t from = decisions_.size() > last ? decisions_.size() - last : 0;
    for (std::size_t i = from; i < decisions_.size(); ++i) out += decisions_[i] + "\n";
    return out.empty() ? "no decisions yet\n" : out;
}

std::string Sim::sky() const {
    std::string out;
    for (const auto& s : skyTracks_) {
        out += std::format("{:<9} {:>6.1f}° el  {:>6.1f}° az", s.name, s.elevation, s.azimuth);
        if (s.relay)
            out += std::format("  beacon Doppler {:+8.0f} Hz{}", s.doppler_hz, s.elevation >= kRelayMaskDeg ? "  << relay link" : "");
        else
            out += s.elevation >= kReconMaskDeg ? "  << imaging the corridor" : "  (the other side's imager)";
        out += "\n";
    }
    return out;
}

std::uint64_t Sim::digest() const {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&](long long v) {
        for (int i = 0; i < 8; ++i) h = (h ^ static_cast<std::uint64_t>((v >> (8 * i)) & 0xff)) * 1099511628211ULL;
    };
    for (const auto& s : shots_) mix(static_cast<int>(s.outcome)), mix(std::llround(s.impactAt * 4));
    for (const auto& u : units_) mix(std::llround(u.pos.x)), mix(std::llround(u.pos.y)), mix(u.alive);
    mix(stats_.hits), mix(std::llround(stats_.worstNavErrorM));
    return h;
}

} // namespace ohflock
