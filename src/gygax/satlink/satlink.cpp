// SatLink geometry (TEME -> Earth-fixed -> topocentric), pass prediction and Hamlib rigctld.

#include <gygax/satlink/satlink.hpp>

#include <gygax/net/link.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <utility>

namespace gygax::satlink {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;
constexpr double kWgs84EccentricitySquared = kWgs84Flattening * (2.0 - kWgs84Flattening);
constexpr double kPassScanStepSeconds = 20.0;
constexpr double kPassScanStepsPerOrbit = 200.0;
constexpr double kPassEdgeToleranceSeconds = 0.01;
constexpr double kPassPeakToleranceSeconds = 0.1;
constexpr int kGeodeticIterations = 6;
constexpr std::size_t kRigMaxReplyBytes = 4096;

struct EnuBasis {
    Vector3 east, north, up;
};

EnuBasis enuBasis(double latitude_deg, double longitude_deg) {
    const double phi = latitude_deg * kDegToRad;
    const double lambda = longitude_deg * kDegToRad;
    const double sp = std::sin(phi), cp = std::cos(phi), sl = std::sin(lambda), cl = std::cos(lambda);
    return {{-sl, cl, 0.0}, {-sp * cl, -sp * sl, cp}, {cp * cl, cp * sl, sp}};
}

// Find where a continuous function crosses zero between a and b (f(a), f(b) of opposite sign).
template <class F> double bisect(F&& f, double a, double b, double fa, double tolerance) {
    while (b - a > tolerance) {
        const double m = 0.5 * (a + b);
        const double fm = f(m);
        if ((fm >= 0.0) == (fa >= 0.0)) {
            a = m;
            fa = fm;
        } else {
            b = m;
        }
    }
    return 0.5 * (a + b);
}

// Golden-section search for the maximum of a unimodal function on [a, b].
template <class F> double goldenMaximum(F&& f, double a, double b, double tolerance) {
    constexpr double invphi = 0.6180339887498949;
    double c = b - invphi * (b - a), d = a + invphi * (b - a);
    double fc = f(c), fd = f(d);
    while (b - a > tolerance) {
        if (fc > fd) {
            b = d;
            d = c;
            fd = fc;
            c = b - invphi * (b - a);
            fc = f(c);
        } else {
            a = c;
            c = d;
            fc = fd;
            d = a + invphi * (b - a);
            fd = f(d);
        }
    }
    return 0.5 * (a + b);
}

} // namespace

double Vector3::norm() const noexcept {
    return std::sqrt(dot(*this));
}

Vector3 Observer::toEcef() const noexcept {
    const double phi = latitude_deg * kDegToRad;
    const double lambda = longitude_deg * kDegToRad;
    const double h = elevation_m / 1000.0;
    const double sp = std::sin(phi), cp = std::cos(phi);
    const double n = kWgs84SemiMajorAxisKm / std::sqrt(1.0 - kWgs84EccentricitySquared * sp * sp);
    return {(n + h) * cp * std::cos(lambda), (n + h) * cp * std::sin(lambda), (n * (1.0 - kWgs84EccentricitySquared) + h) * sp};
}

StateVector temeToEcef(const StateVector& teme, double unix_seconds) noexcept {
    // Rotate by GMST about z, then remove the frame's own rotation from the velocity.
    const double theta = greenwichSiderealRad(unix_seconds);
    const double c = std::cos(theta), s = std::sin(theta);
    const auto& r = teme.position_km;
    const auto& v = teme.velocity_km_s;
    const Vector3 rp{c * r.x + s * r.y, -s * r.x + c * r.y, r.z};
    const Vector3 vp{c * v.x + s * v.y + kEarthRotationRadS * rp.y, -s * v.x + c * v.y - kEarthRotationRadS * rp.x, v.z};
    return {rp, vp};
}

Geodetic ecefToGeodetic(const Vector3& r) noexcept {
    const double p = std::hypot(r.x, r.y);
    double lat = std::atan2(r.z, p * (1.0 - kWgs84EccentricitySquared));
    double n = kWgs84SemiMajorAxisKm;
    for (int i = 0; i < kGeodeticIterations; ++i) {
        const double s = std::sin(lat);
        n = kWgs84SemiMajorAxisKm / std::sqrt(1.0 - kWgs84EccentricitySquared * s * s);
        lat = std::atan2(r.z + kWgs84EccentricitySquared * n * s, p);
    }
    const double s = std::sin(lat), c = std::cos(lat);
    const double h = p * c + (r.z + kWgs84EccentricitySquared * n * s) * s - n;
    double lon = std::atan2(r.y, r.x) * kRadToDeg;
    if (lon >= 180.0) lon -= 360.0;
    return {lat * kRadToDeg, lon, h};
}

Topocentric lookAngles(const Observer& observer, const StateVector& ecef) noexcept {
    const Vector3 rho = ecef.position_km - observer.toEcef();
    const double range = rho.norm();
    if (range <= 0.0) return {};
    const auto basis = enuBasis(observer.latitude_deg, observer.longitude_deg);
    const double e = rho.dot(basis.east), n = rho.dot(basis.north), u = rho.dot(basis.up);
    double az = std::atan2(e, n) * kRadToDeg;
    if (az < 0.0) az += 360.0;
    if (az >= 360.0) az -= 360.0;
    return {std::asin(std::clamp(u / range, -1.0, 1.0)) * kRadToDeg, az, range, ecef.velocity_km_s.dot(rho) / range};
}

double dopplerShiftHz(double carrier_hz, double range_rate_km_s) noexcept {
    return -carrier_hz * range_rate_km_s / kSpeedOfLightKmS;
}

// --- Satellite ----------------------------------------------------------------------------------

Satellite::Satellite(Tle tle, Gravity gravity) : tle_(std::move(tle)), sgp4_(tle_, gravity), epoch_unix_(tle_.epochUnixSeconds()) {}

Satellite::Satellite(std::string_view name, std::string_view line1, std::string_view line2) : Satellite(Tle::parse(line1, line2, name)) {}

StateVector Satellite::teme(double unix_seconds) const {
    return sgp4_.propagate((unix_seconds - epoch_unix_) / 60.0);
}

StateVector Satellite::ecef(double unix_seconds) const {
    return temeToEcef(teme(unix_seconds), unix_seconds);
}

Geodetic Satellite::subpoint(double unix_seconds) const {
    return ecefToGeodetic(ecef(unix_seconds).position_km);
}

Topocentric Satellite::observe(const Observer& observer, double unix_seconds) const {
    return lookAngles(observer, ecef(unix_seconds));
}

double Satellite::dopplerShiftHz(const Observer& observer, double unix_seconds, double carrier_hz) const {
    return satlink::dopplerShiftHz(carrier_hz, observe(observer, unix_seconds).range_rate_km_s);
}

std::vector<Pass> Satellite::passes(const Observer& observer, double start_unix, double end_unix, double min_elevation_deg) const {
    std::vector<Pass> out;
    if (!(end_unix > start_unix)) return out;
    const double step = std::min(kPassScanStepSeconds, tle_.periodMinutes() * 60.0 / kPassScanStepsPerOrbit);
    const auto above = [&](double t) { return observe(observer, t).elevation_deg - min_elevation_deg; };

    // The scan keeps its highest sample in each pass; the peak is refined within a step of it,
    // which holds for any pass length (a geostationary "pass" is not unimodal over a day).
    double bestT = start_unix, bestF = -1e300;
    auto finish = [&](Pass& p) {
        const double lo = std::max(p.rise_unix, bestT - step), hi = std::min(p.set_unix, bestT + step);
        p.culmination_unix = hi > lo ? goldenMaximum(above, lo, hi, kPassPeakToleranceSeconds) : bestT;
        p.max_elevation_deg = observe(observer, p.culmination_unix).elevation_deg;
        p.rise_azimuth_deg = observe(observer, p.rise_unix).azimuth_deg;
        p.set_azimuth_deg = observe(observer, p.set_unix).azimuth_deg;
        out.push_back(p);
    };

    double t0 = start_unix;
    double f0 = above(t0);
    Pass current;
    bool up = f0 >= 0.0;
    if (up) {
        current.rise_unix = start_unix;
        current.rise_clipped = true;
        bestT = t0;
        bestF = f0;
    }
    while (t0 < end_unix) {
        const double t1 = std::min(t0 + step, end_unix);
        const double f1 = above(t1);
        if (!up && f1 >= 0.0) {
            current = Pass{};
            current.rise_unix = bisect(above, t0, t1, f0, kPassEdgeToleranceSeconds);
            up = true;
            bestF = -1e300;
        } else if (up && f1 < 0.0) {
            current.set_unix = bisect(above, t0, t1, f0, kPassEdgeToleranceSeconds);
            finish(current);
            up = false;
        }
        if (up && f1 > bestF) {
            bestT = t1;
            bestF = f1;
        }
        t0 = t1;
        f0 = f1;
    }
    if (up) {
        current.set_unix = end_unix;
        current.set_clipped = true;
        finish(current);
    }
    return out;
}

// --- Hamlib rigctld -----------------------------------------------------------------------------

RigClient::RigClient(std::string host, int port, int timeout_ms) : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {
    if (host_.empty()) host_ = "127.0.0.1";
    if (port_ <= 0 || port_ > 65535) throw RigError(std::format("rigctld port {} is out of range", port_));
    if (timeout_ms_ <= 0) throw RigError("rigctld timeout must be positive");
}

RigClient::~RigClient() = default;
RigClient::RigClient(RigClient&&) noexcept = default;
RigClient& RigClient::operator=(RigClient&&) noexcept = default;

void RigClient::connect() {
    std::string error;
    link_ = net::openTcpLink(host_, static_cast<std::uint16_t>(port_), std::chrono::milliseconds(timeout_ms_), &error);
    pending_.clear();
    if (!link_) throw RigError(std::format("rigctld at {}:{}: {}", host_, port_, error));
}

std::string RigClient::exchange(std::string_view line) {
    if (!link_) connect();
    const std::string wire = std::string(line) + "\n";
    if (link_->write({reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()}) != 0) {
        link_.reset();
        throw RigError(std::format("rigctld at {}:{}: connection lost while sending", host_, port_));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
    std::vector<std::uint8_t> chunk;
    for (;;) {
        if (const auto nl = pending_.find('\n'); nl != std::string::npos) {
            std::string reply = pending_.substr(0, nl);
            pending_.erase(0, nl + 1);
            if (!reply.empty() && reply.back() == '\r') reply.pop_back();
            return reply;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0 || pending_.size() > kRigMaxReplyBytes) {
            link_.reset();
            throw RigError(std::format("rigctld at {}:{} did not answer \"{}\" within {} ms", host_, port_, line, timeout_ms_));
        }
        const int rc = link_->read(chunk, left);
        if (rc == -ETIMEDOUT) continue;
        if (rc != 0) {
            // A reply that ends at a closed connection still counts.
            link_.reset();
            if (!pending_.empty()) {
                std::string reply = std::exchange(pending_, {});
                return reply;
            }
            throw RigError(std::format("rigctld at {}:{}: connection closed ({})", host_, port_, std::strerror(-rc)));
        }
        pending_.append(chunk.begin(), chunk.end());
    }
}

std::string RigClient::command(std::string_view line) {
    if (line.find('\n') != std::string_view::npos) throw RigError("rigctld commands are single lines");
    const bool fresh = !link_;
    try {
        return exchange(line);
    } catch (const RigError&) {
        // A connection rigctld has since closed fails on first use; retry once on a new one.
        if (fresh) throw;
        return exchange(line);
    }
}

void RigClient::setFrequency(double frequency_hz) {
    if (!(frequency_hz > 0.0) || !std::isfinite(frequency_hz)) throw RigError(std::format("invalid frequency {} Hz", frequency_hz));
    const auto reply = command(std::format("F {}", std::llround(frequency_hz)));
    if (reply != "RPRT 0") throw RigError(std::format("rigctld refused F {}: {}", std::llround(frequency_hz), reply));
}

double RigClient::frequency() {
    const auto reply = command("f");
    char* end = nullptr;
    const double hz = std::strtod(reply.c_str(), &end);
    if (end == reply.c_str() || reply.starts_with("RPRT")) throw RigError(std::format("rigctld answered f with \"{}\"", reply));
    return hz;
}

std::string RigClient::setRigFrequency(const std::string& host, int port, double frequency_hz, int timeout_ms) {
    if (!(frequency_hz > 0.0) || !std::isfinite(frequency_hz)) throw RigError(std::format("invalid frequency {} Hz", frequency_hz));
    RigClient client(host, port, timeout_ms);
    return client.command(std::format("F {}", std::llround(frequency_hz)));
}

} // namespace gygax::satlink
