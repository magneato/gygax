#include <gtest/gtest.h>

#include <gygax/satlink/satlink.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace gygax::satlink;

namespace {

// An ISS element set with representative values for January 2024 (checksums valid).
constexpr const char* kIssLine1 = "1 25544U 98067A   24001.50000000  .00016717  00000-0  30306-3 0  9999";
constexpr const char* kIssLine2 = "2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.49815335432866";

// Vanguard 1, the first case of Vallado's verification set.
constexpr const char* kVanguardLine1 = "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753";
constexpr const char* kVanguardLine2 = "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667";

double distance(const Vector3& a, const Vector3& b) {
    return (a - b).norm();
}

std::string fixture(const char* name) {
    return std::string(GYGAX_TEST_SGP4_FIXTURES) + "/" + name;
}

struct Reference {
    double minutes;
    Vector3 r, v;
};

// The verification set's error cases (33333-33335) carry deliberately wrong checksums; they test
// the propagator, not the reader, so recompute them.
std::string withChecksum(std::string line) {
    line = line.substr(0, 68);
    return line + static_cast<char>('0' + tleChecksum(line));
}

// tcppver.out: "<catalog> xx" headers, then "<minutes> x y z vx vy vz ..." rows.
std::map<std::string, std::vector<Reference>> readReference() {
    std::map<std::string, std::vector<Reference>> out;
    std::ifstream in(fixture("tcppver.out"));
    std::string line, current;
    while (std::getline(in, line)) {
        if (line.find("xx") != std::string::npos) {
            std::istringstream(line) >> current;
            current.insert(0, 5 - std::min<std::size_t>(5, current.size()), '0'); // "5" -> "00005", as the TLE has it
            continue;
        }
        std::istringstream row(line);
        Reference ref{};
        if (row >> ref.minutes >> ref.r.x >> ref.r.y >> ref.r.z >> ref.v.x >> ref.v.y >> ref.v.z) out[current].push_back(ref);
    }
    return out;
}

struct VerificationCase {
    Tle tle;
    double start, stop, step;
};

std::vector<VerificationCase> readVerificationTles() {
    std::vector<VerificationCase> out;
    std::ifstream in(fixture("SGP4-VER.TLE"));
    std::string line, line1;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("1 ")) line1 = line;
        if (line.starts_with("2 ")) {
            VerificationCase c{Tle::parse(withChecksum(line1), withChecksum(line)), 0, 0, 0};
            std::istringstream(line.substr(69)) >> c.start >> c.stop >> c.step;
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

TEST(SatLinkTle, ParsesEveryField) {
    const auto t = Tle::parse(kIssLine1, kIssLine2, "ISS (ZARYA)");
    EXPECT_EQ(t.name, "ISS (ZARYA)");
    EXPECT_EQ(t.catalog, "25544");
    EXPECT_EQ(t.classification, 'U');
    EXPECT_EQ(t.international_designator, "98067A");
    EXPECT_EQ(t.epoch_year, 2024);
    EXPECT_DOUBLE_EQ(t.epoch_day, 1.5);
    EXPECT_DOUBLE_EQ(t.mean_motion_dot, 0.00016717);
    EXPECT_NEAR(t.bstar, 0.30306e-3, 1e-15);
    EXPECT_DOUBLE_EQ(t.inclination_deg, 51.6416);
    EXPECT_DOUBLE_EQ(t.raan_deg, 247.4627);
    EXPECT_DOUBLE_EQ(t.eccentricity, 0.0006703);
    EXPECT_DOUBLE_EQ(t.arg_perigee_deg, 130.5360);
    EXPECT_DOUBLE_EQ(t.mean_anomaly_deg, 325.0288);
    EXPECT_DOUBLE_EQ(t.mean_motion_rev_day, 15.49815335);
    EXPECT_EQ(t.revolution, 43286);
    // 2024-01-01 12:00:00 UTC
    EXPECT_DOUBLE_EQ(t.epochUnixSeconds(), 1704110400.0);
    EXPECT_NEAR(t.epochJulianDate(), 2460311.0, 1e-9);
    EXPECT_NEAR(t.periodMinutes(), 92.9143, 1e-3);
}

TEST(SatLinkTle, AcceptsAThreeLineTitleAndNegativeExponents) {
    const auto t = Tle::parse(kVanguardLine1, kVanguardLine2, "0 VANGUARD 1");
    EXPECT_EQ(t.name, "VANGUARD 1");
    EXPECT_EQ(t.epoch_year, 2000);
    EXPECT_NEAR(t.bstar, 0.28098e-4, 1e-15);
}

TEST(SatLinkTle, RejectsCorruptLinesWithTheColumn) {
    std::string bad = kIssLine2;
    bad[20] = '9'; // flip a digit: the checksum no longer adds up
    try {
        (void)Tle::parse(kIssLine1, bad);
        FAIL() << "accepted a line with a bad checksum";
    } catch (const TleError& e) {
        EXPECT_NE(std::string(e.what()).find("checksum"), std::string::npos) << e.what();
    }
    std::string letters = kIssLine2;
    letters[9] = 'x';
    letters[68] = static_cast<char>('0' + tleChecksum(letters));
    try {
        (void)Tle::parse(kIssLine1, letters);
        FAIL() << "accepted a letter in the inclination";
    } catch (const TleError& e) {
        EXPECT_NE(std::string(e.what()).find("columns 9-16 (inclination)"), std::string::npos) << e.what();
    }
    EXPECT_THROW((void)Tle::parse(kIssLine2, kIssLine1), TleError);
    EXPECT_THROW((void)Tle::parse("1 25544U", kIssLine2), TleError);
    std::string other = kVanguardLine2;
    EXPECT_THROW((void)Tle::parse(kIssLine1, other), TleError); // catalog numbers disagree
}

TEST(SatLinkTle, ChecksumCountsMinusSignsAsOne) {
    EXPECT_EQ(tleChecksum(kIssLine1), 9);
    EXPECT_EQ(tleChecksum(kIssLine2), 6);
    EXPECT_EQ(tleChecksum("1 -"), 2);
}

// The heart of it: every satellite and time in Vallado's verification set, near-Earth and deep
// space, resonant and Lyddane cases included, must match the reference output.
TEST(SatLinkSgp4, ReproducesTheVerificationSet) {
    const auto reference = readReference();
    const auto cases = readVerificationTles();
    ASSERT_EQ(cases.size(), 33u);
    std::size_t compared = 0, deep = 0;
    double worstR = 0.0, worstV = 0.0;
    std::set<std::string> done; // 20413 is listed twice (two time spans); its rows are already merged
    for (const auto& c : cases) {
        if (!done.insert(c.tle.catalog).second) continue;
        std::optional<Sgp4> init;
        try {
            init.emplace(c.tle);
        } catch (const TleError&) {
            EXPECT_EQ(reference.count(c.tle.catalog), 0u) << c.tle.catalog << " was rejected but has reference output";
            continue;
        }
        const Sgp4& sgp4 = *init;
        deep += sgp4.deepSpace() ? 1 : 0;
        const auto it = reference.find(c.tle.catalog);
        ASSERT_NE(it, reference.end()) << c.tle.catalog;
        for (const auto& ref : it->second) {
            StateVector s;
            try {
                s = sgp4.propagate(ref.minutes);
            } catch (const Sgp4Error& e) {
                // The reference prints the step at which these cases fail; failing is the right answer.
                if (e.status() == Sgp4Status::Decayed) continue;
                if (c.tle.catalog == "33334" && e.status() == Sgp4Status::PerturbedEccentricity) continue;
                ADD_FAILURE() << c.tle.catalog << " at " << ref.minutes << ": " << e.what();
                continue;
            }
            worstR = std::max(worstR, distance(s.position_km, ref.r));
            worstV = std::max(worstV, distance(s.velocity_km_s, ref.v));
            EXPECT_LT(distance(s.position_km, ref.r), 1e-6) << c.tle.catalog << " at " << ref.minutes << " min";
            EXPECT_LT(distance(s.velocity_km_s, ref.v), 1e-8) << c.tle.catalog << " at " << ref.minutes << " min";
            ++compared;
        }
    }
    EXPECT_GE(compared, 660u);
    EXPECT_GT(deep, 10u);
    RecordProperty("states_compared", std::to_string(compared));
    RecordProperty("worst_position_mm", std::to_string(worstR * 1e6));
    RecordProperty("worst_velocity_mm_s", std::to_string(worstV * 1e6));
}

TEST(SatLinkSgp4, ReportsDecayAndBadOrbitsAsErrors) {
    // 28872 decays within its verification window; 33333 and 33334 cannot be propagated far.
    for (const auto& c : readVerificationTles()) {
        if (c.tle.catalog != "28872") continue;
        const Sgp4 sgp4(c.tle);
        try {
            (void)sgp4.propagate(c.stop);
            FAIL() << "a decayed orbit propagated";
        } catch (const Sgp4Error& e) {
            EXPECT_EQ(e.status(), Sgp4Status::Decayed);
            EXPECT_NE(std::string(e.what()).find("decayed"), std::string::npos);
        }
    }
}

TEST(SatLinkGeometry, SiderealTimeMatchesTheJulianDateFormula) {
    // At J2000.0 GMST is 280.46061837 degrees; one solar day later it has gained ~0.9856 degrees.
    EXPECT_NEAR(greenwichSiderealRad(946728000.0) * 180.0 / std::numbers::pi, 280.46061837, 1e-6);
    EXPECT_NEAR(greenwichSiderealRad(946728000.0 + 86400.0) * 180.0 / std::numbers::pi, 280.46061837 + 0.98564736629, 1e-6);
    // A 1 ms step turns the Earth by 7.29e-8 rad. Going through a Julian date the steps would jitter
    // by ~3e-9 rad (40 us); from Unix seconds only a double's 0.24 us resolution remains (~2e-11 rad).
    const double t = 1791500000.0;
    for (int i = 0; i < 100; ++i) {
        const double step = greenwichSiderealRad(t + i * 1e-3 + 1e-3) - greenwichSiderealRad(t + i * 1e-3);
        EXPECT_NEAR(step, kEarthRotationRadS * 1e-3, 1e-10);
    }
}

TEST(SatLinkGeometry, ObserverEcefMatchesWgs84) {
    // Greenwich observatory, 46 m.
    const Observer g{51.4779, -0.0015, 46.0};
    const auto r = g.toEcef();
    EXPECT_NEAR(r.x, 3980.6, 0.1);
    EXPECT_NEAR(r.y, -0.104, 0.01);
    EXPECT_NEAR(r.z, 4966.86, 0.1);
    const auto back = ecefToGeodetic(r);
    EXPECT_NEAR(back.latitude_deg, 51.4779, 1e-9);
    EXPECT_NEAR(back.longitude_deg, -0.0015, 1e-9);
    EXPECT_NEAR(back.altitude_km, 0.046, 1e-9);
}

TEST(SatLinkGeometry, GeodeticRoundTripsEverywhere) {
    for (double lat = -89.5; lat <= 89.5; lat += 17.9) {
        for (double lon = -179.0; lon < 180.0; lon += 37.0) {
            for (double h : {0.0, 420e3, 35786e3}) {
                const auto g = ecefToGeodetic(Observer{lat, lon, h}.toEcef());
                EXPECT_NEAR(g.latitude_deg, lat, 1e-9);
                EXPECT_NEAR(g.longitude_deg, lon, 1e-9);
                EXPECT_NEAR(g.altitude_km * 1000.0, h, 1e-5);
            }
        }
    }
}

TEST(SatLinkGeometry, LookAnglesPointAtTheZenithAndCompassPoints) {
    const Observer o{40.0, -105.0, 1600.0};
    const auto up = ecefToGeodetic(o.toEcef());
    const Vector3 zenith = Observer{up.latitude_deg, up.longitude_deg, 500e3 + 1600.0}.toEcef();
    const auto look = lookAngles(o, {zenith, {}});
    EXPECT_NEAR(look.elevation_deg, 90.0, 1e-6);
    EXPECT_NEAR(look.range_km, 500.0, 1e-6);

    const auto north = lookAngles(o, {Observer{41.0, -105.0, 1600.0}.toEcef(), {}});
    EXPECT_NEAR(north.azimuth_deg, 0.0, 0.5);
    EXPECT_LT(north.elevation_deg, 0.0); // a point on the ground a degree away is below the horizon
    const auto east = lookAngles(o, {Observer{40.0, -104.0, 1600.0}.toEcef(), {}});
    EXPECT_NEAR(east.azimuth_deg, 90.0, 0.5);
}

TEST(SatLinkSatellite, IssStaysInLowEarthOrbit) {
    const Satellite iss("ISS (ZARYA)", kIssLine1, kIssLine2);
    EXPECT_FALSE(iss.deepSpace());
    const double epoch = iss.tle().epochUnixSeconds();
    for (double dt = 0.0; dt < 86400.0; dt += 600.0) {
        const auto sub = iss.subpoint(epoch + dt);
        // Height above the ellipsoid swings with the orbit's eccentricity and the Earth's flattening,
        // and geodetic latitude peaks a little above the inclination.
        EXPECT_GT(sub.altitude_km, 400.0);
        EXPECT_LT(sub.altitude_km, 445.0);
        EXPECT_LE(std::abs(sub.latitude_deg), 51.9);
        const auto e = iss.ecef(epoch + dt);
        EXPECT_NEAR(e.velocity_km_s.norm(), 7.2, 0.25); // inertial ~7.66 km/s less Earth's rotation
    }
}

TEST(SatLinkSatellite, RangeRateIsTheDerivativeOfRange) {
    const Satellite iss("ISS", kIssLine1, kIssLine2);
    const Observer o{51.4779, -0.0015, 46.0};
    const double t = iss.tle().epochUnixSeconds() + 3 * 3600.0;
    for (double dt = 0.0; dt < 6000.0; dt += 97.0) {
        const auto look = iss.observe(o, t + dt);
        const double h = 0.01;
        const double numeric = (iss.observe(o, t + dt + h).range_km - iss.observe(o, t + dt - h).range_km) / (2 * h);
        // SGP4's velocity is its own analytic series, not the exact derivative of its position;
        // they agree to a few cm/s, a fraction of a hertz at UHF.
        EXPECT_NEAR(look.range_rate_km_s, numeric, 5e-5) << "at +" << dt << " s";
    }
}

TEST(SatLinkSatellite, DopplerRisesHighAndSetsLow) {
    const Satellite iss("ISS", kIssLine1, kIssLine2);
    const Observer o{51.4779, -0.0015, 46.0};
    const double start = iss.tle().epochUnixSeconds();
    const auto passes = iss.passes(o, start, start + 2 * 86400.0, 10.0);
    ASSERT_FALSE(passes.empty());
    constexpr double kCarrier = 437.8e6;
    for (const auto& p : passes) {
        const double rise = iss.dopplerShiftHz(o, p.rise_unix, kCarrier);
        const double peak = iss.dopplerShiftHz(o, p.culmination_unix, kCarrier);
        const double set = iss.dopplerShiftHz(o, p.set_unix, kCarrier);
        EXPECT_GT(rise, 3000.0);           // approaching: received high
        EXPECT_LT(set, -3000.0);           // receding: received low
        EXPECT_LT(std::abs(peak), 2000.0); // near zero at closest approach
        EXPECT_LT(rise, 11000.0);          // 7.7 km/s at 437.8 MHz cannot exceed ~11.2 kHz
    }
}

TEST(SatLinkSatellite, PassesAreOrderedRefinedAndAboveTheMask) {
    const Satellite iss("ISS", kIssLine1, kIssLine2);
    const Observer o{51.4779, -0.0015, 46.0};
    const double start = iss.tle().epochUnixSeconds();
    const auto passes = iss.passes(o, start, start + 3 * 86400.0, 10.0);
    ASSERT_GE(passes.size(), 6u);
    double last = start;
    for (const auto& p : passes) {
        EXPECT_GT(p.rise_unix, last);
        EXPECT_LT(p.rise_unix, p.culmination_unix);
        EXPECT_LT(p.culmination_unix, p.set_unix);
        EXPECT_GE(p.max_elevation_deg, 10.0);
        EXPECT_LE(p.max_elevation_deg, 90.0);
        EXPECT_GT(p.durationSeconds(), 0.0);
        EXPECT_LT(p.durationSeconds(), 12 * 60.0);
        EXPECT_NEAR(iss.observe(o, p.rise_unix).elevation_deg, 10.0, 1e-3);
        EXPECT_NEAR(iss.observe(o, p.set_unix).elevation_deg, 10.0, 1e-3);
        EXPECT_GE(p.max_elevation_deg, iss.observe(o, p.culmination_unix - 5).elevation_deg);
        EXPECT_GE(p.max_elevation_deg, iss.observe(o, p.culmination_unix + 5).elevation_deg);
        last = p.set_unix;
    }
}

TEST(SatLinkSatellite, ADeepSpaceOrbitIsUpAllDay) {
    // Molniya-like 12-hour orbits and geostationary ones go through SDP4.
    for (const auto& c : readVerificationTles()) {
        if (c.tle.catalog != "28626") continue; // a geostationary satellite in the set
        const Satellite geo(c.tle);
        EXPECT_TRUE(geo.deepSpace());
        const double t = c.tle.epochUnixSeconds();
        const auto sub = geo.subpoint(t);
        EXPECT_NEAR(sub.altitude_km, 35786.0, 100.0);
        EXPECT_LT(std::abs(sub.latitude_deg), 1.0);
        const Observer under{0.0, sub.longitude_deg, 0.0};
        const auto passes = geo.passes(under, t, t + 86400.0, 0.0);
        ASSERT_EQ(passes.size(), 1u);
        EXPECT_TRUE(passes[0].rise_clipped);
        EXPECT_TRUE(passes[0].set_clipped);
        EXPECT_GT(passes[0].max_elevation_deg, 85.0);
        return;
    }
    FAIL() << "geostationary case missing from the verification set";
}

namespace {

// A loopback stand-in for rigctld that answers each line from a script.
class FakeRigctld {
public:
    explicit FakeRigctld(std::vector<std::string> replies, bool closeAfterEach = false) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(fd_, 4);
        socklen_t len = sizeof(addr);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this, replies = std::move(replies), closeAfterEach] {
            std::size_t next = 0;
            while (next < replies.size()) {
                const int c = ::accept(fd_, nullptr, nullptr);
                if (c < 0) return;
                client_.store(c);
                std::string buffer;
                char chunk[256];
                while (next < replies.size()) {
                    const auto n = ::recv(c, chunk, sizeof(chunk), 0);
                    if (n <= 0) break;
                    buffer.append(chunk, static_cast<std::size_t>(n));
                    bool closed = false;
                    for (auto nl = buffer.find('\n'); nl != std::string::npos && next < replies.size(); nl = buffer.find('\n')) {
                        received_.push_back(buffer.substr(0, nl));
                        buffer.erase(0, nl + 1);
                        const auto reply = replies[next++] + "\n";
                        ::send(c, reply.data(), reply.size(), 0);
                        if (closeAfterEach) {
                            closed = true;
                            break;
                        }
                    }
                    if (closed) break;
                }
                client_.store(-1);
                ::close(c);
            }
        });
    }

    ~FakeRigctld() { (void)finish(); }

    // Stop serving and return every line received, in order.
    std::vector<std::string> finish() {
        if (thread_.joinable()) {
            ::shutdown(fd_, SHUT_RDWR);
            if (const int c = client_.load(); c >= 0) ::shutdown(c, SHUT_RDWR);
            thread_.join();
            ::close(fd_);
        }
        return received_;
    }

    [[nodiscard]] int port() const { return port_; }

private:
    int fd_{-1};
    int port_{0};
    std::atomic<int> client_{-1};
    std::vector<std::string> received_;
    std::thread thread_;
};

} // namespace

TEST(SatLinkRig, RetunesOverOneConnection) {
    FakeRigctld rig({"RPRT 0", "RPRT 0", "437801234"});
    RigClient client("127.0.0.1", rig.port(), 2000);
    client.setFrequency(437800000.4);
    client.setFrequency(437801234.0);
    EXPECT_DOUBLE_EQ(client.frequency(), 437801234.0);
    EXPECT_EQ(rig.finish(), (std::vector<std::string>{"F 437800000", "F 437801234", "f"}));
}

TEST(SatLinkRig, ReportsRefusalsAndReconnects) {
    FakeRigctld rig({"RPRT 0", "RPRT -11"}, true);
    RigClient client("127.0.0.1", rig.port(), 2000);
    client.setFrequency(145800000.0); // the server hangs up after answering
    try {
        client.setFrequency(145801000.0); // so this one needs a fresh connection
        FAIL() << "a refusal was accepted";
    } catch (const RigError& e) {
        EXPECT_NE(std::string(e.what()).find("RPRT -11"), std::string::npos) << e.what();
    }
}

TEST(SatLinkRig, OneShotHelperReturnsTheReply) {
    FakeRigctld rig({"RPRT 0", "RPRT -1"}, true);
    EXPECT_EQ(RigClient::setRigFrequency("127.0.0.1", rig.port(), 437800000.0), "RPRT 0");
    EXPECT_EQ(RigClient::setRigFrequency("127.0.0.1", rig.port(), 145800000.0), "RPRT -1");
    EXPECT_EQ(rig.finish(), (std::vector<std::string>{"F 437800000", "F 145800000"}));
}

TEST(SatLinkRig, UnreachableRigctldIsAnError) {
    // Bind a port and close it again: nothing listens there now.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(fd);
    RigClient client("127.0.0.1", ntohs(addr.sin_port), 500);
    EXPECT_THROW(client.setFrequency(145800000.0), RigError);
    EXPECT_THROW(RigClient("127.0.0.1", 70000), RigError);
}
