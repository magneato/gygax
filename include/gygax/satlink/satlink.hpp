#pragma once

// SatLink: satellite tracking for radio operators and ground stations.
//
//   Tle         a parsed, checksum-verified NORAD two-line element set
//   Sgp4        the SGP4/SDP4 propagator (Vallado et al. 2006, "Revisiting Spacetrack Report #3"),
//               near-Earth and deep-space, verified against the reference vectors in tcppver.out
//   Satellite   a Tle plus its propagator: TEME state, Earth-fixed state, sub-satellite point,
//               look angles, exact range rate, Doppler, and rise/culminate/set pass prediction
//   RigClient   a Hamlib rigctld client that keeps one TCP connection open across retunes
//
// Frames: SGP4 produces TEME (true equator, mean equinox). Earth-fixed coordinates are TEME
// rotated by Greenwich mean sidereal time (IAU-82, as SGP4 itself uses), with UT1 taken as UTC
// and polar motion ignored. That costs at most about 0.4 km on the ground, under 0.05 degrees of
// antenna pointing for a satellite 500 km away, far inside any antenna beam.
//
// Time is Unix seconds (UTC, POSIX: leap seconds are not counted), the clock a station already has.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gygax::net {
class ByteLink;
}

namespace gygax::satlink {

inline constexpr double kSpeedOfLightKmS = 299792.458;
inline constexpr double kWgs84SemiMajorAxisKm = 6378.137;
inline constexpr double kWgs84Flattening = 1.0 / 298.257223563;
inline constexpr double kEarthRotationRadS = 7.292115146706979e-5;
inline constexpr int kHamlibDefaultPort = 4532;

struct Vector3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};

    [[nodiscard]] constexpr double dot(const Vector3& o) const noexcept { return x * o.x + y * o.y + z * o.z; }
    [[nodiscard]] constexpr Vector3 cross(const Vector3& o) const noexcept {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    [[nodiscard]] double norm() const noexcept;
    [[nodiscard]] constexpr Vector3 operator+(const Vector3& o) const noexcept { return {x + o.x, y + o.y, z + o.z}; }
    [[nodiscard]] constexpr Vector3 operator-(const Vector3& o) const noexcept { return {x - o.x, y - o.y, z - o.z}; }
    [[nodiscard]] constexpr Vector3 operator*(double s) const noexcept { return {x * s, y * s, z * s}; }
};

// Position (km) and velocity (km/s) in one frame.
struct StateVector {
    Vector3 position_km;
    Vector3 velocity_km_s;
};

// --- Two-line elements --------------------------------------------------------------------------

class TleError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// The fields of a two-line element set, in the units the TLE prints them.
struct Tle {
    std::string name;
    std::string catalog; // five characters; Alpha-5 ("A0001") accepted
    char classification{'U'};
    std::string international_designator;
    int epoch_year{0};            // four digits (57..99 -> 19xx, 00..56 -> 20xx)
    double epoch_day{0.0};        // day of year, 1.0 = 1 January 00:00 UTC
    double mean_motion_dot{0.0};  // rev/day^2, first derivative / 2 (informational; SGP4 ignores it)
    double mean_motion_ddot{0.0}; // rev/day^3, second derivative / 6 (likewise)
    double bstar{0.0};            // drag term, 1/earth radii
    int element_set{0};
    double inclination_deg{0.0};
    double raan_deg{0.0}; // right ascension of the ascending node
    double eccentricity{0.0};
    double arg_perigee_deg{0.0};
    double mean_anomaly_deg{0.0};
    double mean_motion_rev_day{0.0};
    long revolution{0};

    // Parse and validate two lines (and an optional title line). Throws TleError naming the
    // offending line and column. A checksum digit in column 69 must match.
    [[nodiscard]] static Tle parse(std::string_view line1, std::string_view line2, std::string_view name = {});

    [[nodiscard]] double epochJulianDate() const noexcept;
    [[nodiscard]] double epochUnixSeconds() const noexcept;
    [[nodiscard]] double periodMinutes() const noexcept;
};

// The TLE checksum of a line: the sum of its digits, with each '-' counting 1, over columns 1-68, mod 10.
[[nodiscard]] int tleChecksum(std::string_view line) noexcept;

// --- SGP4 / SDP4 --------------------------------------------------------------------------------

// The propagator's own error conditions, numbered as in the reference implementation.
enum class Sgp4Status : int {
    Ok = 0,
    MeanEccentricity = 1,      // mean eccentricity outside [0, 1)
    MeanMotion = 2,            // mean motion not positive
    PerturbedEccentricity = 3, // eccentricity outside [0, 1] after lunar-solar periodics
    SemiLatusRectum = 4,       // semi-latus rectum negative
    Decayed = 6,               // orbit radius below one Earth radius
};

[[nodiscard]] std::string_view describe(Sgp4Status status) noexcept;

class Sgp4Error : public std::runtime_error {
public:
    Sgp4Error(Sgp4Status status, double minutes);
    [[nodiscard]] Sgp4Status status() const noexcept { return status_; }

private:
    Sgp4Status status_;
};

// TLEs are fitted with WGS-72 constants, so that is the default; WGS-84 is offered for comparison.
enum class Gravity { Wgs72, Wgs84 };

class Sgp4 {
public:
    explicit Sgp4(const Tle& tle, Gravity gravity = Gravity::Wgs72);
    ~Sgp4();
    Sgp4(const Sgp4& other);
    Sgp4& operator=(const Sgp4& other);
    Sgp4(Sgp4&&) noexcept;
    Sgp4& operator=(Sgp4&&) noexcept;

    // TEME state at `minutes` after the element epoch. Thread-safe; throws Sgp4Error.
    [[nodiscard]] StateVector propagate(double minutes) const;

    // True for SDP4: periods of 225 minutes or more get lunar-solar and resonance terms.
    [[nodiscard]] bool deepSpace() const noexcept;

    struct Elements; // the initialised element record (kept private to the implementation)

private:
    std::unique_ptr<Elements> e_;
};

// --- Ground geometry ----------------------------------------------------------------------------

// A point on or above the WGS-84 ellipsoid.
struct Geodetic {
    double latitude_deg{0.0};  // north positive
    double longitude_deg{0.0}; // east positive, [-180, 180)
    double altitude_km{0.0};   // above the ellipsoid
};

// A ground station.
struct Observer {
    double latitude_deg{0.0};
    double longitude_deg{0.0};
    double elevation_m{0.0}; // above the WGS-84 ellipsoid

    [[nodiscard]] Vector3 toEcef() const noexcept;
};

// What a station sees: where to point, how far, and how fast the distance is changing.
struct Topocentric {
    double elevation_deg{0.0};   // above the local horizon
    double azimuth_deg{0.0};     // from true north, clockwise, [0, 360)
    double range_km{0.0};        // slant range
    double range_rate_km_s{0.0}; // positive while receding
};

// One visibility window above a minimum elevation.
struct Pass {
    double rise_unix{0.0};
    double culmination_unix{0.0};
    double set_unix{0.0};
    double max_elevation_deg{0.0};
    double rise_azimuth_deg{0.0};
    double set_azimuth_deg{0.0};
    bool rise_clipped{false}; // already up when the search window opened
    bool set_clipped{false};  // still up when it closed

    [[nodiscard]] double durationSeconds() const noexcept { return set_unix - rise_unix; }
};

[[nodiscard]] double unixToJulianDate(double unix_seconds) noexcept;
[[nodiscard]] double julianDateToUnix(double julian_date) noexcept;
// Greenwich mean sidereal time (IAU-82, radians in [0, 2pi)), the rotation SGP4's TEME frame expects.
[[nodiscard]] double greenwichSiderealRad(double unix_seconds) noexcept;
[[nodiscard]] StateVector temeToEcef(const StateVector& teme, double unix_seconds) noexcept;
[[nodiscard]] Geodetic ecefToGeodetic(const Vector3& ecef_km) noexcept;
[[nodiscard]] Topocentric lookAngles(const Observer& observer, const StateVector& ecef) noexcept;
// First-order Doppler shift of a carrier for a given range rate: -f * rdot / c.
[[nodiscard]] double dopplerShiftHz(double carrier_hz, double range_rate_km_s) noexcept;

// --- Satellites ---------------------------------------------------------------------------------

class Satellite {
public:
    explicit Satellite(Tle tle, Gravity gravity = Gravity::Wgs72);
    Satellite(std::string_view name, std::string_view line1, std::string_view line2);

    [[nodiscard]] const Tle& tle() const noexcept { return tle_; }
    [[nodiscard]] const std::string& name() const noexcept { return tle_.name; }
    [[nodiscard]] bool deepSpace() const noexcept { return sgp4_.deepSpace(); }

    // Every query below takes Unix seconds and throws Sgp4Error if the elements cannot reach it.
    [[nodiscard]] StateVector teme(double unix_seconds) const;
    [[nodiscard]] StateVector ecef(double unix_seconds) const;
    [[nodiscard]] Geodetic subpoint(double unix_seconds) const;
    [[nodiscard]] Topocentric observe(const Observer& observer, double unix_seconds) const;
    // Shift of a downlink carrier as received at the observer (add it to tune a receiver; subtract
    // it from an uplink to have the satellite hear the nominal frequency).
    [[nodiscard]] double dopplerShiftHz(const Observer& observer, double unix_seconds, double carrier_hz) const;

    // Passes above `min_elevation_deg` between two times, in order. Rise and set are refined to
    // 10 ms and culmination to 0.1 s; the scan step is 20 s (or 1/200 of the orbit if that is
    // shorter), so a pass briefer than one step may be missed.
    [[nodiscard]] std::vector<Pass> passes(const Observer& observer, double start_unix, double end_unix,
                                           double min_elevation_deg = 0.0) const;

private:
    Tle tle_;
    Sgp4 sgp4_;
    double epoch_unix_{0.0};
};

// --- Radio control ------------------------------------------------------------------------------

class RigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Hamlib rigctld over TCP (default port 4532). Connects on first use and reconnects once if the
// connection has dropped; not thread-safe (give each thread its own client).
class RigClient {
public:
    explicit RigClient(std::string host = "127.0.0.1", int port = kHamlibDefaultPort, int timeout_ms = 2000);
    ~RigClient();
    RigClient(RigClient&&) noexcept;
    RigClient& operator=(RigClient&&) noexcept;

    // Send one command line and return the first reply line, without its newline.
    [[nodiscard]] std::string command(std::string_view line);

    // `F <hz>`; throws RigError unless rigctld answers "RPRT 0".
    void setFrequency(double frequency_hz);
    // `f`; the current VFO frequency in Hz.
    [[nodiscard]] double frequency();

    [[nodiscard]] const std::string& host() const noexcept { return host_; }
    [[nodiscard]] int port() const noexcept { return port_; }

    // One connection, one `F <hz>`, rigctld's reply returned as text (e.g. "RPRT 0"). Throws RigError
    // only when rigctld cannot be reached or does not answer.
    [[nodiscard]] static std::string setRigFrequency(const std::string& host, int port, double frequency_hz, int timeout_ms = 2000);

private:
    void connect();
    [[nodiscard]] std::string exchange(std::string_view line);

    std::string host_;
    int port_;
    int timeout_ms_;
    std::shared_ptr<net::ByteLink> link_;
    std::string pending_;
};

} // namespace gygax::satlink
