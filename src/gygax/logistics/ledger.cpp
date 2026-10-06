#include <gygax/logistics/ledger.hpp>

#include <sys/random.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>

namespace gygax::logistics {

namespace {

constexpr double kEarthRadiusM = 6371008.8;
constexpr std::size_t kMaxIntegerTokenDigits = 9;
constexpr std::size_t kMaxAttributeCount = 16;
constexpr std::size_t kGuidTextLengthCharacters = 36;
constexpr std::size_t kMaxTokenCharacters = 64;
constexpr std::size_t kMaxLineTokenCount = 40;
constexpr std::size_t kMaxSiteNameCharacters = 128;
constexpr std::size_t kMaxJournalNoteCharacters = 512;
constexpr std::size_t kGuidByteCount = 16;
constexpr std::size_t kDateTimeComponentCount = 6;
constexpr std::int64_t kEpochMillisecondsThreshold = 100000000000LL;
constexpr std::int64_t kMillisecondsPerSecond = 1000;
constexpr std::int64_t kSecondsPerMinute = 60;
constexpr std::int64_t kSecondsPerHour = 3600;
constexpr std::int64_t kSecondsPerDay = 86400;
constexpr int kMinimumSupportedYear = 1970;
constexpr double kMinimumLatitudeDegrees = -90.0;
constexpr double kMaximumLatitudeDegrees = 90.0;
constexpr double kMinimumLongitudeDegrees = -180.0;
constexpr double kMaximumLongitudeDegrees = 180.0;
constexpr double kMaximumModelRangeMeters = 1e7;
constexpr double kMaximumCruiseSpeedMetersPerSecond = 1000.0;
constexpr std::size_t kGuidVersionByteIndex = 6;
constexpr std::size_t kGuidVariantByteIndex = 8;
constexpr unsigned char kGuidVersionMask = 0x0F;
constexpr unsigned char kGuidVersion4Value = 0x40;
constexpr unsigned char kGuidVariantMask = 0x3F;
constexpr unsigned char kGuidRfc4122Variant = 0x80;

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

double rad(double d) {
    return d * std::numbers::pi / 180.0;
}
double deg(double r) {
    return r * 180.0 / std::numbers::pi;
}

Outcome failure(int status, std::string code, std::string message) {
    Outcome o;
    o.ok = false;
    o.status = status;
    o.code = std::move(code);
    o.message = std::move(message);
    return o;
}

bool parseInteger(std::string_view s, std::int64_t& out) {
    if (s.empty()) return false;
    std::size_t i = 0;
    bool negative = false;
    if (s[0] == '+' || s[0] == '-') {
        negative = s[0] == '-';
        i = 1;
    }
    if (i >= s.size() || s.size() - i > kMaxIntegerTokenDigits) return false;
    std::int64_t v = 0;
    for (; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        v = v * 10 + (s[i] - '0');
    }
    out = negative ? -v : v;
    return true;
}

json::Value attrsJson(const Attributes& attrs) {
    json::Value o = json::Value::object();
    for (const auto& [k, v] : attrs) o[k] = v;
    return o;
}

bool attrsFrom(const json::Value* v, Attributes& out, std::string* error) {
    if (v == nullptr || v->isNull()) return true;
    if (!v->isObject()) {
        *error = "attrs must be an object of strings";
        return false;
    }
    for (const auto& [k, val] : v->asObject()) {
        if (!validToken(k) || !val.isString() || !validToken(val.asString())) {
            *error = "invalid attribute '" + k + "'";
            return false;
        }
        out[k] = val.asString();
    }
    if (out.size() > kMaxAttributeCount) {
        *error = "at most 16 attributes";
        return false;
    }
    return true;
}

std::vector<std::string> stringList(const json::Value* v) {
    std::vector<std::string> out;
    if (v == nullptr || !v->isArray()) return out;
    for (const auto& item : v->asArray()) {
        if (item.isString()) out.push_back(item.asString());
    }
    return out;
}

} // namespace

std::string newGuid() {
    unsigned char b[kGuidByteCount];
    std::size_t got = 0;
    while (got < sizeof(b)) {
        const auto n = getrandom(b + got, sizeof(b) - got, 0);
        if (n < 0) throw std::runtime_error("getrandom failed");
        got += static_cast<std::size_t>(n);
    }
    b[kGuidVersionByteIndex] = static_cast<unsigned char>((b[kGuidVersionByteIndex] & kGuidVersionMask) | kGuidVersion4Value);
    b[kGuidVariantByteIndex] = static_cast<unsigned char>((b[kGuidVariantByteIndex] & kGuidVariantMask) | kGuidRfc4122Variant);
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(kGuidTextLengthCharacters);
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex[b[i] >> 4]);
        out.push_back(hex[b[i] & 15]);
    }
    return out;
}

bool withinRange(double value, double low, double high) {
    return value >= low && value <= high;
}

bool validGuid(std::string_view t) {
    if (t.size() != kGuidTextLengthCharacters) return false;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? t[i] != '-' : !std::isxdigit(static_cast<unsigned char>(t[i]))) return false;
    }
    return true;
}

bool validToken(std::string_view t) {
    if (t.empty() || t.size() > kMaxTokenCharacters) return false;
    return std::all_of(t.begin(), t.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-' || c == ':' || c == '/';
    });
}

bool parseLine(std::string_view line, ParsedLine& out, std::string* error) {
    auto fail = [&](std::string m) {
        if (error != nullptr) *error = std::move(m);
        return false;
    };
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        const auto start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i > start) tokens.emplace_back(line.substr(start, i - start));
    }
    if (tokens.size() < 2) return fail("expected '<+/-count> <sku> [key=value ...]'");
    if (tokens.size() > kMaxLineTokenCount) return fail("too many tokens");
    ParsedLine p;
    if (!parseInteger(tokens[0], p.delta) || p.delta == 0) return fail("count must be a non-zero integer such as -3 or +8");
    if (p.delta > Ledger::kMaxDelta || p.delta < -Ledger::kMaxDelta) return fail("count out of range");
    if (!validToken(tokens[1])) return fail("invalid sku");
    p.sku = tokens[1];
    for (std::size_t t = 2; t < tokens.size(); ++t) {
        const auto eq = tokens[t].find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == tokens[t].size()) return fail("expected key=value, got '" + tokens[t] + "'");
        const auto key = tokens[t].substr(0, eq);
        const auto value = tokens[t].substr(eq + 1);
        if (key == "guid" || key == "guids") {
            std::size_t pos = 0;
            while (pos <= value.size()) {
                const auto comma = value.find(',', pos);
                const auto piece = value.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if (!validGuid(piece)) return fail("invalid guid '" + piece + "'");
                p.guids.push_back(piece);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        } else if (key == "reason") {
            if (!validToken(value)) return fail("invalid reason");
            p.reason = value;
        } else if (key == "site") {
            if (!validToken(value)) return fail("invalid site");
            p.site = value;
        } else {
            if (!validToken(key) || !validToken(value)) return fail("invalid attribute '" + tokens[t] + "'");
            p.attrs[key] = value;
        }
    }
    if (p.attrs.size() > 16) return fail("at most 16 attributes");
    out = std::move(p);
    return true;
}

std::string formatLine(std::int64_t delta, const std::string& sku, const Attributes& attrs) {
    std::string out = (delta > 0 ? "+" : "") + std::to_string(delta) + " " + sku;
    for (const auto& [k, v] : attrs) {
        out += ' ';
        out += k;
        out += '=';
        out += v;
    }
    return out;
}

std::optional<std::int64_t> parseSince(const json::Value& value, std::int64_t now) {
    if (value.isNull()) return 0;
    if (value.isInt()) {
        const auto v = value.asInt();
        if (v < 0) return std::nullopt;
        return v < kEpochMillisecondsThreshold ? v * kMillisecondsPerSecond : v;
    }
    if (!value.isString()) return std::nullopt;
    const auto& s = value.asString();
    if (s.empty()) return 0;
    std::int64_t n = 0;
    if (parseInteger(s, n) && n >= 0) return n < kEpochMillisecondsThreshold ? n * kMillisecondsPerSecond : n;
    if (s.size() >= 2 && parseInteger(std::string_view(s).substr(0, s.size() - 1), n) && n >= 0) {
        std::int64_t unit = 0;
        switch (s.back()) {
        case 'm': unit = kSecondsPerMinute * kMillisecondsPerSecond; break;
        case 'h': unit = kSecondsPerHour * kMillisecondsPerSecond; break;
        case 'd': unit = kSecondsPerDay * kMillisecondsPerSecond; break;
        case 'w': unit = 7 * kSecondsPerDay * kMillisecondsPerSecond; break;
        default: break;
        }
        if (unit != 0) return std::max<std::int64_t>(0, now - n * unit);
    }
    std::array<int, kDateTimeComponentCount> parts{};
    std::size_t at = 0;
    std::size_t count = 0;
    const char separators[] = {'-', '-', 'T', ':', ':', 'Z'};
    while (count < kDateTimeComponentCount && at < s.size()) {
        const auto [end, ec] = std::from_chars(s.data() + at, s.data() + s.size(), parts[count]);
        if (ec != std::errc()) return std::nullopt;
        at = static_cast<std::size_t>(end - s.data());
        ++count;
        if (at == s.size()) break;
        if (s[at] != separators[count - 1]) return std::nullopt;
        ++at;
    }
    const bool dateOnly = count == 3 && at == s.size();
    const bool full = count == kDateTimeComponentCount && at == s.size() && s.back() == 'Z';
    if ((dateOnly || full) && parts[0] >= kMinimumSupportedYear && parts[1] >= 1 && parts[1] <= 12 && parts[2] >= 1 && parts[2] <= 31) {
        std::tm tm{};
        tm.tm_year = parts[0] - 1900;
        tm.tm_mon = parts[1] - 1;
        tm.tm_mday = parts[2];
        tm.tm_hour = dateOnly ? 0 : parts[3];
        tm.tm_min = dateOnly ? 0 : parts[4];
        tm.tm_sec = dateOnly ? 0 : parts[5];
        const auto t = timegm(&tm);
        if (t >= 0) return static_cast<std::int64_t>(t) * kMillisecondsPerSecond;
    }
    return std::nullopt;
}

double distanceMeters(const Point& a, const Point& b) {
    const double dLat = rad(b.latitude - a.latitude);
    const double dLon = rad(b.longitude - a.longitude);
    const double h = std::sin(dLat / 2) * std::sin(dLat / 2) +
                     std::cos(rad(a.latitude)) * std::cos(rad(b.latitude)) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2.0 * kEarthRadiusM * std::asin(std::min(1.0, std::sqrt(h)));
}

Point pointToward(const Point& from, const Point& to, double meters) {
    const double lat1 = rad(from.latitude);
    const double lon1 = rad(from.longitude);
    const double lat2 = rad(to.latitude);
    const double dLon = rad(to.longitude - from.longitude);
    const double bearing =
        std::atan2(std::sin(dLon) * std::cos(lat2), std::cos(lat1) * std::sin(lat2) - std::sin(lat1) * std::cos(lat2) * std::cos(dLon));
    const double ang = meters / kEarthRadiusM;
    const double lat = std::asin(std::sin(lat1) * std::cos(ang) + std::cos(lat1) * std::sin(ang) * std::cos(bearing));
    const double lon =
        lon1 + std::atan2(std::sin(bearing) * std::sin(ang) * std::cos(lat1), std::cos(ang) - std::sin(lat1) * std::sin(lat));
    return {deg(lat), std::fmod(deg(lon) + 540.0, 360.0) - 180.0};
}

namespace {

json::Value pointJson(const Point& p) {
    json::Value o = json::Value::object();
    o["latitude"] = p.latitude;
    o["longitude"] = p.longitude;
    return o;
}

} // namespace

json::Value planRoute(const PlanRequest& req, const std::vector<Site>& allSites) {
    json::Value out = json::Value::object();
    json::Value legs = json::Value::array();
    auto finish = [&](bool feasible, const std::string& reason, double total, int stops, double fuelM) {
        out["feasible"] = feasible;
        out["legs"] = std::move(legs);
        out["total_distance_m"] = total;
        out["refuel_stops"] = stops;
        if (feasible) {
            out["final_fuel_fraction"] = req.maxRangeM > 0 ? std::clamp(fuelM / req.maxRangeM, 0.0, 1.0) : 0.0;
            if (req.cruiseMps > 0) out["eta_seconds"] = total / req.cruiseMps;
        } else {
            out["reason"] = reason;
        }
        return out;
    };
    if (!(req.maxRangeM > 0.0)) return finish(false, "max_range_m must be positive", 0, 0, 0);
    if (req.waypoints.empty() && !req.returnTo) return finish(false, "at least one waypoint is required", 0, 0, 0);
    const double reserveM = req.maxRangeM * std::clamp(req.reserveFraction, 0.0, 0.9);
    const double fullUsable = req.maxRangeM - reserveM;
    double fuelM = req.maxRangeM * std::clamp(req.fuelFraction, 0.0, 1.0);

    std::vector<Site> sites;
    for (const auto& s : allSites) {
        if (s.services.empty() || req.power.empty() || std::find(s.services.begin(), s.services.end(), req.power) != s.services.end())
            sites.push_back(s);
    }

    std::vector<Point> targets = req.waypoints;
    const std::size_t returnIndex = targets.size();
    if (req.returnTo) targets.push_back(*req.returnTo);

    Point pos = req.origin;
    double total = 0.0;
    int stops = 0;
    const double inf = std::numeric_limits<double>::infinity();

    for (std::size_t ti = 0; ti < targets.size(); ++ti) {
        const std::size_t n = sites.size() + 2;
        const std::size_t target = n - 1;
        std::vector<Point> pts(n);
        pts[0] = pos;
        for (std::size_t s = 0; s < sites.size(); ++s) pts[s + 1] = sites[s].position;
        pts[target] = targets[ti];
        const double startUsable = std::max(0.0, fuelM - reserveM);
        double needAfter = 0.0;
        if (ti + 1 < targets.size()) {
            needAfter = distanceMeters(targets[ti], targets[ti + 1]);
            for (const auto& s : sites) needAfter = std::min(needAfter, distanceMeters(targets[ti], s.position));
        }
        auto limit = [&](std::size_t u) { return u == 0 ? startUsable : fullUsable; };
        std::vector<double> dist(n, inf);
        std::vector<std::ptrdiff_t> prev(n, -1);
        std::vector<bool> done(n, false);
        dist[0] = 0.0;
        for (std::size_t iter = 0; iter < n; ++iter) {
            std::size_t u = n;
            for (std::size_t k = 0; k < n; ++k) {
                if (!done[k] && dist[k] < inf && (u == n || dist[k] < dist[u])) u = k;
            }
            if (u == n) break;
            done[u] = true;
            if (u == target) break;
            for (std::size_t v = 0; v < n; ++v) {
                if (v == u || done[v] || v == 0) continue;
                const double d = distanceMeters(pts[u], pts[v]);
                const double need = v == target ? needAfter : 0.0;
                if (d + need <= limit(u) + 1e-3 && dist[u] + d < dist[v]) {
                    dist[v] = dist[u] + d;
                    prev[v] = static_cast<std::ptrdiff_t>(u);
                }
            }
        }
        if (dist[target] == inf) {
            std::size_t best = 0;
            double bestD = inf;
            for (std::size_t k = 0; k < n - 1; ++k) {
                if (dist[k] == inf) continue;
                const double d = distanceMeters(pts[k], pts[target]);
                if (d < bestD) {
                    bestD = d;
                    best = k;
                }
            }
            json::Value blocked = json::Value::object();
            blocked["waypoint_index"] = ti;
            blocked["is_return"] = req.returnTo && ti == returnIndex;
            out["blocked"] = std::move(blocked);
            json::Value suggestion = json::Value::object();
            suggestion["add_refuel_point"] = pointJson(pointToward(pts[best], pts[target], std::max(0.0, limit(best))));
            suggestion["reason"] = "furthest position reachable toward the blocked waypoint";
            out["suggestion"] = std::move(suggestion);
            return finish(false, "no refuel chain reaches waypoint " + std::to_string(ti), total, stops, fuelM);
        }
        std::vector<std::size_t> path;
        for (std::ptrdiff_t v = static_cast<std::ptrdiff_t>(target); v != -1; v = prev[static_cast<std::size_t>(v)])
            path.push_back(static_cast<std::size_t>(v));
        std::reverse(path.begin(), path.end());
        for (std::size_t h = 1; h < path.size(); ++h) {
            const auto u = path[h - 1];
            const auto v = path[h];
            const double d = distanceMeters(pts[u], pts[v]);
            fuelM -= d;
            total += d;
            json::Value leg = json::Value::object();
            leg["from"] = pointJson(pts[u]);
            leg["to"] = pointJson(pts[v]);
            leg["distance_m"] = d;
            leg["fuel_remaining_m"] = std::max(0.0, fuelM);
            if (v == target) {
                leg["type"] = req.returnTo && ti == returnIndex ? "return" : "waypoint";
                leg["waypoint_index"] = ti;
            } else {
                leg["type"] = "refuel";
                leg["site"] = sites[v - 1].id;
                fuelM = req.maxRangeM;
                ++stops;
            }
            legs.push(std::move(leg));
        }
        pos = targets[ti];
    }
    return finish(true, {}, total, stops, fuelM);
}

Ledger::Ledger(std::string journalPath) : journalPath_(std::move(journalPath)) {
    if (journalPath_.empty()) return;
    replay();
    const int fd = ::open(journalPath_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("cannot open ledger journal " + journalPath_);
    journalFile_ = ::fdopen(fd, "a");
    if (journalFile_ == nullptr) {
        ::close(fd);
        throw std::runtime_error("cannot open ledger journal " + journalPath_);
    }
    std::ifstream in(journalPath_, std::ios::binary | std::ios::ate);
    if (in && in.tellg() > 0) {
        in.seekg(-1, std::ios::end);
        if (in.get() != '\n') std::fputc('\n', static_cast<std::FILE*>(journalFile_));
    }
}

Ledger::~Ledger() {
    if (journalFile_ != nullptr) std::fclose(static_cast<std::FILE*>(journalFile_));
}

void Ledger::journal(const json::Value& entry) {
    if (journalFile_ == nullptr || replaying_) return;
    auto* f = static_cast<std::FILE*>(journalFile_);
    const auto line = entry.dump() + "\n";
    if (std::fwrite(line.data(), 1, line.size(), f) != line.size() || std::fflush(f) != 0)
        throw std::runtime_error("ledger journal write failed");
    ::fsync(::fileno(f));
}

void Ledger::replay() {
    std::ifstream in(journalPath_);
    if (!in) return;
    std::vector<std::string> lines;
    std::vector<std::streamoff> offsets;
    for (std::string l;;) {
        const auto at = in.tellg();
        if (!std::getline(in, l)) break;
        if (l.empty()) continue;
        offsets.push_back(at);
        lines.push_back(std::move(l));
    }
    in.close();
    replaying_ = true;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto parsed = json::parse(lines[i]);
        if (!parsed || !parsed->isObject()) {
            if (i + 1 == lines.size()) {
                if (::truncate(journalPath_.c_str(), offsets[i]) != 0) throw std::runtime_error("cannot repair ledger journal");
                break;
            }
            throw std::runtime_error("ledger journal is corrupt at line " + std::to_string(i + 1));
        }
        const auto op = parsed->getString("op");
        Outcome o;
        if (op == "model") {
            o = putModel(*parsed);
        } else if (op == "site") {
            o = putSite(*parsed);
        } else if (op == "site_remove") {
            o = removeSite(parsed->getString("id"));
        } else if (op == "unit") {
            const auto* patch = parsed->find("patch");
            o = updateUnit(parsed->getString("guid"), patch != nullptr ? *patch : json::Value::object());
        } else if (op == "event") {
            Event e;
            e.id = parsed->getString("id");
            e.timeMs = parsed->getInt("time_ms");
            e.sku = parsed->getString("sku");
            e.delta = parsed->getInt("delta");
            e.guids = stringList(parsed->find("guids"));
            e.reason = parsed->getString("reason");
            e.site = parsed->getString("site");
            e.note = parsed->getString("note");
            std::string err;
            attrsFrom(parsed->find("attrs"), e.attrs, &err);
            applyEvent(e);
        } else {
            o = failure(400, "journal", "unknown op");
        }
        if (!o.ok) {
            replaying_ = false;
            throw std::runtime_error("ledger journal entry " + std::to_string(i + 1) + " rejected: " + o.message);
        }
    }
    replaying_ = false;
}

json::Value Ledger::toJson(const Unit& u) {
    json::Value o = json::Value::object();
    o["guid"] = u.guid;
    o["sku"] = u.sku;
    o["status"] = u.status;
    o["attrs"] = attrsJson(u.attrs);
    if (!u.device.empty()) o["device"] = u.device;
    if (!u.site.empty()) o["site"] = u.site;
    o["fuel"] = u.fuel;
    if (u.position) {
        o["latitude"] = u.position->latitude;
        o["longitude"] = u.position->longitude;
    }
    o["created_ms"] = u.createdMs;
    if (u.endedMs != 0) {
        o["ended_ms"] = u.endedMs;
        o["end_reason"] = u.endReason;
    }
    return o;
}

json::Value Ledger::toJson(const Event& e) {
    json::Value o = json::Value::object();
    o["id"] = e.id;
    o["time_ms"] = e.timeMs;
    o["sku"] = e.sku;
    o["delta"] = e.delta;
    json::Value g = json::Value::array();
    for (const auto& id : e.guids) g.push(id);
    o["guids"] = std::move(g);
    o["reason"] = e.reason;
    if (!e.site.empty()) o["site"] = e.site;
    if (!e.note.empty()) o["note"] = e.note;
    o["attrs"] = attrsJson(e.attrs);
    o["line"] = formatLine(e.delta, e.sku, e.attrs);
    return o;
}

json::Value Ledger::toJson(const Model& m) {
    json::Value o = json::Value::object();
    o["sku"] = m.sku;
    o["kind"] = m.kind;
    o["power"] = m.power;
    o["max_range_m"] = m.maxRangeM;
    o["cruise_mps"] = m.cruiseMps;
    o["attrs"] = attrsJson(m.attrs);
    return o;
}

json::Value Ledger::toJson(const Site& s) {
    json::Value o = json::Value::object();
    o["id"] = s.id;
    o["name"] = s.name;
    o["kind"] = s.kind;
    o["latitude"] = s.position.latitude;
    o["longitude"] = s.position.longitude;
    json::Value services = json::Value::array();
    for (const auto& x : s.services) services.push(x);
    o["services"] = std::move(services);
    return o;
}

bool Ledger::matches(const Attributes& have, const Attributes& want) {
    return std::all_of(want.begin(), want.end(), [&](const auto& kv) {
        const auto it = have.find(kv.first);
        return it != have.end() && it->second == kv.second;
    });
}

Outcome Ledger::putModel(const json::Value& spec) {
    Model m;
    m.sku = spec.getString("sku");
    if (!validToken(m.sku)) return failure(400, "invalid_request", "'sku' must be 1-64 characters of [A-Za-z0-9._:/-]");
    m.kind = spec.getString("kind");
    m.power = spec.getString("power");
    if ((!m.kind.empty() && !validToken(m.kind)) || (!m.power.empty() && !validToken(m.power)))
        return failure(400, "invalid_request", "invalid kind or power");
    m.maxRangeM = spec.getDouble("max_range_m", 0.0);
    m.cruiseMps = spec.getDouble("cruise_mps", 0.0);
    if (!withinRange(m.maxRangeM, 0.0, kMaximumModelRangeMeters) || !withinRange(m.cruiseMps, 0.0, kMaximumCruiseSpeedMetersPerSecond))
        return failure(400, "invalid_request", "max_range_m must be within 0..1e7 and cruise_mps within 0..1000");
    std::string err;
    if (!attrsFrom(spec.find("attrs"), m.attrs, &err)) return failure(400, "invalid_request", err);
    std::lock_guard lock(mutex_);
    if (!models_.contains(m.sku) && models_.size() >= kMaxModels) return failure(409, "limit", "too many models");
    auto entry = toJson(m);
    entry["op"] = "model";
    journal(entry);
    models_[m.sku] = m;
    Outcome o;
    o.status = 201;
    o.body = toJson(m);
    return o;
}

Outcome Ledger::putSite(const json::Value& spec) {
    Site s;
    s.id = spec.getString("id");
    if (!validToken(s.id)) return failure(400, "invalid_request", "'id' must be 1-64 characters of [A-Za-z0-9._:/-]");
    s.name = spec.getString("name", s.id);
    if (s.name.size() > kMaxSiteNameCharacters) return failure(400, "invalid_request", "name too long");
    s.kind = spec.getString("kind", "refuel");
    static const std::set<std::string> kinds = {"depot", "refuel", "charge", "factory", "hub"};
    if (!kinds.contains(s.kind)) return failure(400, "invalid_request", "kind must be depot, refuel, charge, factory or hub");
    const auto* lat = spec.find("latitude");
    const auto* lon = spec.find("longitude");
    if (lat == nullptr || lon == nullptr || !lat->isNumber() || !lon->isNumber())
        return failure(400, "invalid_request", "latitude and longitude are required");
    s.position = {lat->asDouble(), lon->asDouble()};
    if (!withinRange(s.position.latitude, kMinimumLatitudeDegrees, kMaximumLatitudeDegrees) ||
        !withinRange(s.position.longitude, kMinimumLongitudeDegrees, kMaximumLongitudeDegrees))
        return failure(400, "invalid_request", "latitude or longitude out of range");
    for (const auto& x : stringList(spec.find("services"))) {
        if (!validToken(x)) return failure(400, "invalid_request", "invalid service name");
        s.services.push_back(x);
    }
    std::lock_guard lock(mutex_);
    if (!sites_.contains(s.id) && sites_.size() >= kMaxSites) return failure(409, "limit", "too many sites");
    auto entry = toJson(s);
    entry["op"] = "site";
    journal(entry);
    sites_[s.id] = s;
    Outcome o;
    o.status = 201;
    o.body = toJson(s);
    return o;
}

Outcome Ledger::removeSite(const std::string& id) {
    std::lock_guard lock(mutex_);
    if (!sites_.contains(id)) return failure(404, "not_found", "no such site");
    json::Value entry = json::Value::object();
    entry["op"] = "site_remove";
    entry["id"] = id;
    journal(entry);
    sites_.erase(id);
    return {};
}

Outcome Ledger::updateUnit(const std::string& guid, const json::Value& patch) {
    std::lock_guard lock(mutex_);
    auto it = units_.find(guid);
    if (it == units_.end()) return failure(404, "not_found", "no such unit");
    Unit u = it->second;
    if (const auto* d = patch.find("device")) {
        if (!d->isString() || (!d->asString().empty() && !validToken(d->asString())))
            return failure(400, "invalid_request", "invalid device");
        u.device = d->asString();
    }
    if (const auto* s = patch.find("site")) {
        if (!s->isString() || (!s->asString().empty() && !sites_.contains(s->asString()))) return failure(404, "not_found", "no such site");
        u.site = s->asString();
    }
    if (const auto* f = patch.find("fuel")) {
        if (!f->isNumber() || f->asDouble() < 0.0 || f->asDouble() > 1.0)
            return failure(400, "invalid_request", "fuel must be within 0..1");
        u.fuel = f->asDouble();
    }
    const auto* lat = patch.find("latitude");
    const auto* lon = patch.find("longitude");
    if ((lat == nullptr) != (lon == nullptr)) return failure(400, "invalid_request", "latitude and longitude go together");
    if (lat != nullptr) {
        if (!lat->isNumber() || !lon->isNumber() || std::abs(lat->asDouble()) > kMaximumLatitudeDegrees ||
            std::abs(lon->asDouble()) > kMaximumLongitudeDegrees)
            return failure(400, "invalid_request", "invalid position");
        u.position = Point{lat->asDouble(), lon->asDouble()};
    }
    json::Value entry = json::Value::object();
    entry["op"] = "unit";
    entry["guid"] = guid;
    entry["patch"] = patch;
    journal(entry);
    it->second = u;
    Outcome o;
    o.body = toJson(u);
    return o;
}

void Ledger::applyEvent(const Event& e) {
    if (e.delta > 0) {
        for (const auto& g : e.guids) {
            auto it = units_.find(g);
            if (it != units_.end()) {
                it->second.status = "active";
                it->second.endedMs = 0;
                it->second.endReason.clear();
                continue;
            }
            Unit u;
            u.guid = g;
            u.sku = e.sku;
            u.status = "active";
            u.attrs = e.attrs;
            u.site = e.site;
            u.createdMs = e.timeMs;
            units_[g] = std::move(u);
        }
    } else {
        const bool lost = e.reason.empty() || e.reason == "lost";
        for (const auto& g : e.guids) {
            auto it = units_.find(g);
            if (it == units_.end()) continue;
            it->second.status = lost ? "lost" : "retired";
            it->second.endedMs = e.timeMs;
            it->second.endReason = e.reason.empty() ? "lost" : e.reason;
        }
    }
    events_.push_back(e);
}

Outcome Ledger::record(const json::Value& request) {
    std::lock_guard lock(mutex_);
    return recordLocked(request, false);
}

Outcome Ledger::recordLocked(const json::Value& request, bool) {
    ParsedLine p;
    std::string err;
    if (const auto* line = request.find("line"); line != nullptr && line->isString()) {
        if (!parseLine(line->asString(), p, &err)) return failure(400, "invalid_line", err);
    } else {
        p.delta = request.getInt("delta", 0);
        p.sku = request.getString("sku");
        p.reason = request.getString("reason");
        p.site = request.getString("site");
        if (p.delta == 0 || p.delta > kMaxDelta || p.delta < -kMaxDelta)
            return failure(400, "invalid_request", "'delta' must be a non-zero integer within +-100000");
        if (!validToken(p.sku)) return failure(400, "invalid_request", "'sku' must be 1-64 characters of [A-Za-z0-9._:/-]");
        if ((!p.reason.empty() && !validToken(p.reason)) || (!p.site.empty() && !validToken(p.site)))
            return failure(400, "invalid_request", "invalid reason or site");
        if (!attrsFrom(request.find("attrs"), p.attrs, &err)) return failure(400, "invalid_request", err);
        p.guids = stringList(request.find("guids"));
        for (const auto& g : p.guids) {
            if (!validGuid(g)) return failure(400, "invalid_request", "invalid guid '" + g + "'");
        }
    }
    if (const auto* extra = request.find("attrs"); extra != nullptr && request.find("line") != nullptr) {
        if (!attrsFrom(extra, p.attrs, &err)) return failure(400, "invalid_request", err);
    }
    if (const auto* g = request.find("guids"); g != nullptr && request.find("line") != nullptr) {
        for (const auto& id : stringList(g)) {
            if (!validGuid(id)) return failure(400, "invalid_request", "invalid guid '" + id + "'");
            p.guids.push_back(id);
        }
    }
    const auto note = request.getString("note");
    if (note.size() > kMaxJournalNoteCharacters) return failure(400, "invalid_request", "note too long");
    if (!p.site.empty() && !sites_.contains(p.site)) return failure(404, "not_found", "no such site '" + p.site + "'");
    std::set<std::string> distinct(p.guids.begin(), p.guids.end());
    if (distinct.size() != p.guids.size()) return failure(400, "invalid_request", "duplicate guids");

    Event e;
    e.id = newGuid();
    e.timeMs = nowMs();
    e.sku = p.sku;
    e.delta = p.delta;
    e.reason = p.reason;
    e.site = p.site;
    e.note = note;
    e.attrs = p.attrs;
    const auto count = static_cast<std::size_t>(std::llabs(p.delta));

    if (p.delta > 0) {
        if (!p.guids.empty() && p.guids.size() != count) return failure(400, "invalid_request", "guid count must equal the delta");
        std::size_t fresh = 0;
        for (const auto& g : p.guids) {
            auto it = units_.find(g);
            if (it == units_.end()) {
                ++fresh;
            } else if (it->second.status == "active" || it->second.sku != p.sku) {
                return failure(409, "conflict", "unit " + g + " is already tracked");
            }
        }
        if (p.guids.empty()) fresh = count;
        if (units_.size() + fresh > kMaxUnits) return failure(409, "limit", "unit limit reached");
        e.guids = p.guids;
        while (e.guids.size() < count) e.guids.push_back(newGuid());
        if (e.reason.empty()) e.reason = "produced";
    } else {
        if (!p.guids.empty()) {
            if (p.guids.size() != count) return failure(400, "invalid_request", "guid count must equal the delta");
            for (const auto& g : p.guids) {
                auto it = units_.find(g);
                if (it == units_.end() || it->second.sku != p.sku)
                    return failure(404, "not_found", "no tracked unit " + g + " for sku " + p.sku);
                if (it->second.status != "active") return failure(409, "conflict", "unit " + g + " is not active");
            }
            e.guids = p.guids;
        } else {
            std::vector<const Unit*> candidates;
            for (const auto& [g, u] : units_) {
                if (u.sku == p.sku && u.status == "active" && matches(u.attrs, p.attrs)) candidates.push_back(&u);
            }
            if (candidates.size() < count) {
                auto o = failure(409, "insufficient_stock", "only " + std::to_string(candidates.size()) + " active units match");
                o.body["available"] = candidates.size();
                return o;
            }
            std::sort(candidates.begin(), candidates.end(), [](const Unit* a, const Unit* b) {
                return a->createdMs != b->createdMs ? a->createdMs < b->createdMs : a->guid < b->guid;
            });
            for (std::size_t i = 0; i < count; ++i) e.guids.push_back(candidates[i]->guid);
        }
        if (e.reason.empty()) e.reason = "lost";
    }
    auto entry = toJson(e);
    entry["op"] = "event";
    entry.object_ref().erase("line");
    journal(entry);
    applyEvent(e);
    Outcome o;
    o.status = 201;
    o.body = toJson(e);
    return o;
}

json::Value Ledger::models() const {
    std::lock_guard lock(mutex_);
    json::Value arr = json::Value::array();
    for (const auto& [k, m] : models_) arr.push(toJson(m));
    return arr;
}

json::Value Ledger::sitesJson() const {
    std::lock_guard lock(mutex_);
    json::Value arr = json::Value::array();
    for (const auto& [k, s] : sites_) arr.push(toJson(s));
    return arr;
}

std::vector<Site> Ledger::sites() const {
    std::lock_guard lock(mutex_);
    std::vector<Site> out;
    out.reserve(sites_.size());
    for (const auto& [k, s] : sites_) out.push_back(s);
    return out;
}

std::optional<Model> Ledger::model(const std::string& sku) const {
    std::lock_guard lock(mutex_);
    auto it = models_.find(sku);
    if (it == models_.end()) return std::nullopt;
    return it->second;
}

json::Value Ledger::events(const std::string& sku, std::int64_t sinceMs, std::size_t limit) const {
    std::lock_guard lock(mutex_);
    std::vector<const Event*> picked;
    for (const auto& e : events_) {
        if (e.timeMs >= sinceMs && (sku.empty() || e.sku == sku)) picked.push_back(&e);
    }
    const std::size_t first = picked.size() > limit ? picked.size() - limit : 0;
    json::Value arr = json::Value::array();
    for (std::size_t i = first; i < picked.size(); ++i) arr.push(toJson(*picked[i]));
    return arr;
}

json::Value Ledger::units(const std::string& sku, const std::string& status, std::size_t limit) const {
    std::lock_guard lock(mutex_);
    json::Value arr = json::Value::array();
    std::size_t n = 0;
    for (const auto& [g, u] : units_) {
        if ((!sku.empty() && u.sku != sku) || (!status.empty() && u.status != status)) continue;
        if (n++ >= limit) break;
        arr.push(toJson(u));
    }
    return arr;
}

std::optional<json::Value> Ledger::unit(const std::string& guid) const {
    std::lock_guard lock(mutex_);
    auto it = units_.find(guid);
    if (it == units_.end()) return std::nullopt;
    return toJson(it->second);
}

json::Value Ledger::summary(std::int64_t sinceMs) const {
    std::lock_guard lock(mutex_);
    struct Row {
        std::uint64_t active = 0, lost = 0, retired = 0, gained = 0, lostInWindow = 0;
    };
    std::map<std::string, Row> rows;
    for (const auto& [g, u] : units_) {
        auto& r = rows[u.sku];
        if (u.status == "active")
            ++r.active;
        else if (u.status == "lost")
            ++r.lost;
        else
            ++r.retired;
    }
    struct Flow {
        std::int64_t delta = 0;
        std::uint64_t events = 0;
        Attributes attrs;
        std::string sku;
    };
    std::map<std::string, Flow> flows;
    std::uint64_t inWindow = 0;
    for (const auto& e : events_) {
        if (e.timeMs < sinceMs) continue;
        ++inWindow;
        auto& r = rows[e.sku];
        const auto mag = static_cast<std::uint64_t>(std::llabs(e.delta));
        if (e.delta > 0)
            r.gained += mag;
        else
            r.lostInWindow += mag;
        const auto key = std::string(e.delta > 0 ? "+" : "-") + formatLine(0, e.sku, e.attrs);
        auto& f = flows[key];
        f.delta += e.delta;
        ++f.events;
        f.attrs = e.attrs;
        f.sku = e.sku;
    }
    json::Value out = json::Value::object();
    out["since_ms"] = sinceMs;
    out["generated_ms"] = nowMs();
    out["events_in_window"] = inWindow;
    std::uint64_t active = 0, lost = 0, retired = 0;
    json::Value skus = json::Value::array();
    for (const auto& [sku, r] : rows) {
        active += r.active;
        lost += r.lost;
        retired += r.retired;
        json::Value o = json::Value::object();
        o["sku"] = sku;
        o["active"] = r.active;
        o["lost"] = r.lost;
        o["retired"] = r.retired;
        o["gained_in_window"] = r.gained;
        o["lost_in_window"] = r.lostInWindow;
        if (auto m = models_.find(sku); m != models_.end()) o["model"] = toJson(m->second);
        skus.push(std::move(o));
    }
    json::Value totals = json::Value::object();
    totals["active"] = active;
    totals["lost"] = lost;
    totals["retired"] = retired;
    out["totals"] = std::move(totals);
    out["skus"] = std::move(skus);
    json::Value flow = json::Value::array();
    for (const auto& [key, f] : flows) {
        json::Value o = json::Value::object();
        o["line"] = formatLine(f.delta, f.sku, f.attrs);
        o["delta"] = f.delta;
        o["sku"] = f.sku;
        o["attrs"] = attrsJson(f.attrs);
        o["events"] = f.events;
        flow.push(std::move(o));
    }
    out["flow"] = std::move(flow);
    return out;
}

std::vector<Ledger::Gauge> Ledger::gauges() const {
    std::lock_guard lock(mutex_);
    std::map<std::pair<std::string, std::string>, std::uint64_t> counts;
    for (const auto& [g, u] : units_) ++counts[{u.sku, u.status}];
    std::vector<Gauge> out;
    out.reserve(counts.size());
    for (const auto& [k, n] : counts) out.push_back({k.first, k.second, n});
    return out;
}

std::uint64_t Ledger::eventCount() const {
    std::lock_guard lock(mutex_);
    return events_.size();
}

}
