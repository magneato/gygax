#include <gygax/intel/osint.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

#include <gygax/net/http.hpp>

namespace gygax::intel {

namespace {

constexpr double kEarthRadiusMeters = 6371008.8;
constexpr double kPi = 3.14159265358979323846;
constexpr double kVerifiedConfidence = 0.8;
constexpr double kMillisecondEpochThreshold = 1e11;
constexpr double kMismatchRadiusFactor = 5.0;
constexpr double kEditedSoftwarePenalty = 0.15;
constexpr double kScoreAdvisory = 0.5;
constexpr double kScoreElevated = 2.0;
constexpr double kScoreCritical = 5.0;
constexpr double kMaxConfidence = 0.99;

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

double toRadians(double deg) {
    return deg * kPi / 180.0;
}

int clampInt(std::int64_t v, int lo, int hi) {
    return static_cast<int>(std::clamp<std::int64_t>(v, lo, hi));
}

bool validCoordinate(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) && std::abs(lat) <= 90.0 && std::abs(lon) <= 180.0;
}

// Fixed points that are the same everywhere; surveys add their own with addSite or loadSites.
const Site kBuiltinSites[] = {
    {"Royal Observatory Greenwich", 51.4769, -0.0005, 150.0},
    {"Eiffel Tower", 48.8584, 2.2945, 150.0},
    {"Statue of Liberty", 40.6892, -74.0445, 150.0},
    {"Sydney Opera House", -33.8568, 151.2153, 150.0},
    {"Colosseum", 41.8902, 12.4922, 150.0},
};

std::int64_t timeFromJson(const json::Value* v) {
    if (v == nullptr) return 0;
    if (v->isNumber()) {
        const double t = v->asDouble();
        return static_cast<std::int64_t>(t > kMillisecondEpochThreshold ? t / 1000.0 : t);
    }
    if (v->isString()) {
        if (const auto t = parseIsoTime(v->asString())) return *t;
    }
    return 0;
}

} // namespace

const char* levelName(Level level) {
    switch (level) {
    case Level::Local: return "local";
    case Level::Regional: return "regional";
    case Level::National: return "national";
    case Level::Theater: return "theater";
    case Level::Global: return "global";
    }
    return "local";
}

const char* domainName(Domain domain) {
    switch (domain) {
    case Domain::Infrastructure: return "infrastructure";
    case Domain::Maritime: return "maritime";
    case Domain::Cyber: return "cyber";
    case Domain::Aerospace: return "aerospace";
    case Domain::Geophysical: return "geophysical";
    case Domain::Security: return "security";
    case Domain::Other: return "other";
    }
    return "other";
}

const char* threatName(Threat threat) {
    switch (threat) {
    case Threat::Clear: return "CLEAR";
    case Threat::Advisory: return "ADVISORY";
    case Threat::Elevated: return "ELEVATED";
    case Threat::Critical: return "CRITICAL";
    }
    return "CLEAR";
}

std::optional<Level> parseLevel(std::string_view text) {
    const std::string t = lower(text);
    for (int i = 0; i < kLevelCount; ++i) {
        if (t == levelName(static_cast<Level>(i))) return static_cast<Level>(i);
    }
    return std::nullopt;
}

std::optional<Domain> parseDomain(std::string_view text) {
    const std::string t = lower(text);
    for (int i = 0; i <= static_cast<int>(Domain::Other); ++i) {
        if (t == domainName(static_cast<Domain>(i))) return static_cast<Domain>(i);
    }
    return std::nullopt;
}

double haversineMeters(double lat1, double lon1, double lat2, double lon2) {
    const double dLat = toRadians(lat2 - lat1);
    const double dLon = toRadians(lon2 - lon1);
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
                     std::cos(toRadians(lat1)) * std::cos(toRadians(lat2)) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2.0 * kEarthRadiusMeters * std::asin(std::min(1.0, std::sqrt(a)));
}

std::optional<std::int64_t> parseIsoTime(std::string_view text) {
    const std::string s(text);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    char sep = 0;
    const int n = std::sscanf(s.c_str(), "%d-%d-%d%c%d:%d:%d", &y, &mo, &d, &sep, &h, &mi, &sec);
    if (n < 3) return std::nullopt;
    if (n < 7) {
        h = mi = sec = 0;
        if (n != 3) return std::nullopt;
    } else if (sep != 'T' && sep != ' ') {
        return std::nullopt;
    }
    if (y < 1970 || y > 9999 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 60) {
        return std::nullopt;
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    return static_cast<std::int64_t>(timegm(&tm));
}

OsintEngine::OsintEngine() : OsintEngine(FeedOptions{}) {}

OsintEngine::OsintEngine(FeedOptions options) : options_(options) {
    for (const auto& site : kBuiltinSites) sites_.push_back(site);
}

std::int64_t OsintEngine::clock(std::int64_t now) const {
    if (now != 0) return now;
    if (options_.now != 0) return options_.now;
    return static_cast<std::int64_t>(std::time(nullptr));
}

void OsintEngine::addEvent(Event event) {
    std::lock_guard lock(mutex_);
    if (event.id.empty()) event.id = "evt-" + std::to_string(events_.size() + 1);
    event.severity = std::clamp(event.severity, 1, 5);
    event.confidence = std::clamp(event.confidence, 0.0, 1.0);
    events_.push_back(std::move(event));
}

void OsintEngine::addSite(Site site) {
    std::lock_guard lock(mutex_);
    sites_.push_back(std::move(site));
}

std::size_t OsintEngine::loadSites(const json::Value& sites) {
    const json::Value* list = &sites;
    if (const auto* inner = sites.find("sites")) list = inner;
    std::size_t taken = 0;
    for (const auto& s : list->asArray()) {
        Site site;
        site.name = s.getString("name");
        site.latitude = s.getDouble("latitude", 1000.0);
        site.longitude = s.getDouble("longitude", 1000.0);
        site.radiusMeters = s.getDouble("radius_m", 150.0);
        if (site.name.empty() || !validCoordinate(site.latitude, site.longitude) || !(site.radiusMeters > 0.0)) continue;
        addSite(std::move(site));
        ++taken;
    }
    return taken;
}

std::size_t OsintEngine::loadJson(const json::Value& feed, std::string* error, std::size_t* skipped) {
    std::size_t taken = 0;
    std::size_t bad = 0;
    auto take = [&](Event e) {
        if (e.title.empty() || !validCoordinate(e.latitude, e.longitude)) {
            ++bad;
            return;
        }
        addEvent(std::move(e));
        ++taken;
    };

    if (feed.getString("type") == "FeatureCollection") {
        const auto* features = feed.find("features");
        if (features == nullptr || !features->isArray()) {
            if (error != nullptr) *error = "GeoJSON has no features array";
            return 0;
        }
        for (const auto& f : features->asArray()) {
            const auto* geometry = f.find("geometry");
            const auto* props = f.find("properties");
            const auto* coords = geometry != nullptr ? geometry->find("coordinates") : nullptr;
            if (geometry == nullptr || props == nullptr || coords == nullptr || !coords->isArray() || coords->asArray().size() < 2 ||
                geometry->getString("type") != "Point") {
                ++bad;
                continue;
            }
            Event e;
            e.id = f.getString("id");
            e.longitude = coords->asArray()[0].asDouble(1000.0);
            e.latitude = coords->asArray()[1].asDouble(1000.0);
            e.title = props->getString("title", props->getString("place"));
            e.summary = props->getString("type");
            const double mag = props->getDouble("mag", 0.0);
            e.domain = Domain::Geophysical;
            e.severity = mag < 2.0 ? 1 : mag < 4.0 ? 2 : mag < 5.0 ? 3 : mag < 6.0 ? 4 : 5;
            e.level = mag < 4.5 ? Level::Local : mag < 6.0 ? Level::Regional : mag < 7.0 ? Level::National : Level::Theater;
            e.radiusKm = std::max(1.0, mag * 10.0);
            e.source = props->getString("net", "geojson");
            e.confidence = 0.9;
            e.time = timeFromJson(props->find("time"));
            take(std::move(e));
        }
        if (skipped != nullptr) *skipped = bad;
        return taken;
    }

    const json::Value* list = &feed;
    if (const auto* inner = feed.find("events")) list = inner;
    if (!list->isArray()) {
        if (error != nullptr) *error = "feed must be an array, an {\"events\": []} object or a GeoJSON FeatureCollection";
        return 0;
    }
    for (const auto& j : list->asArray()) {
        Event e;
        e.id = j.getString("id");
        e.title = j.getString("title");
        e.summary = j.getString("summary");
        e.latitude = j.getDouble("latitude", 1000.0);
        e.longitude = j.getDouble("longitude", 1000.0);
        e.radiusKm = std::max(0.0, j.getDouble("radius_km", 0.0));
        e.severity = clampInt(j.getInt("severity", 1), 1, 5);
        e.source = j.getString("source", "feed");
        e.confidence = std::clamp(j.getDouble("confidence", 0.5), 0.0, 1.0);
        e.time = timeFromJson(j.find("time"));
        if (const auto* lv = j.find("level")) {
            if (lv->isString()) {
                const auto parsed = parseLevel(lv->asString());
                if (!parsed) {
                    ++bad;
                    continue;
                }
                e.level = *parsed;
            } else if (lv->isNumber()) {
                e.level = static_cast<Level>(clampInt(lv->asInt(), 0, kLevelCount - 1));
            }
        }
        if (const auto* dm = j.find("domain"); dm != nullptr && dm->isString()) {
            e.domain = parseDomain(dm->asString()).value_or(Domain::Other);
        }
        take(std::move(e));
    }
    if (skipped != nullptr) *skipped = bad;
    return taken;
}

std::size_t OsintEngine::loadFile(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error != nullptr) *error = "cannot open " + path;
        return 0;
    }
    std::ostringstream text;
    text << in.rdbuf();
    std::string parseError;
    auto doc = json::parse(text.str(), &parseError);
    if (!doc) {
        if (error != nullptr) *error = path + ": " + parseError;
        return 0;
    }
    return loadJson(*doc, error);
}

std::size_t OsintEngine::fetch(const std::string& url, std::string* error) {
    std::string urlError;
    const auto parsed = net::Url::parse(url, &urlError);
    if (!parsed) {
        if (error != nullptr) *error = urlError;
        return 0;
    }
    const auto response = net::httpRequest(*parsed, "GET", {}, {{"Accept", "application/json"}});
    if (!response.ok || response.status != 200) {
        if (error != nullptr) *error = response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error;
        return 0;
    }
    std::string parseError;
    auto doc = json::parse(response.body, &parseError);
    if (!doc) {
        if (error != nullptr) *error = "feed is not JSON: " + parseError;
        return 0;
    }
    return loadJson(*doc, error);
}

std::size_t OsintEngine::eventCount() const {
    std::lock_guard lock(mutex_);
    return events_.size();
}

double OsintEngine::confidenceLocked(const Event& event) const {
    // Best confidence per independent source among reports of the same kind from the same place and time.
    std::map<std::string, double> bySource;
    bySource[event.source] = event.confidence;
    for (const auto& other : events_) {
        if (&other == &event || other.domain != event.domain || other.source == event.source) continue;
        if (std::abs(static_cast<double>(other.time - event.time)) > options_.corroborationHours * 3600.0) continue;
        if (haversineMeters(event.latitude, event.longitude, other.latitude, other.longitude) > options_.corroborationKm * 1000.0) continue;
        auto& best = bySource[other.source];
        best = std::max(best, other.confidence);
    }
    double miss = 1.0;
    for (const auto& [source, c] : bySource) miss *= 1.0 - c;
    return std::min(kMaxConfidence, 1.0 - miss);
}

double OsintEngine::confidenceOf(const Event& event) const {
    std::lock_guard lock(mutex_);
    return confidenceLocked(event);
}

double OsintEngine::weightLocked(const Event& event, std::int64_t now) const {
    const double ageHours = std::max(0.0, static_cast<double>(now - event.time) / 3600.0);
    return event.severity * confidenceLocked(event) * std::pow(0.5, ageHours / options_.halfLifeHours);
}

std::vector<Event> OsintEngine::feed(Level level, std::optional<Domain> domain) const {
    std::lock_guard lock(mutex_);
    std::vector<Event> out;
    for (const auto& e : events_) {
        if (domain && e.domain != *domain) continue;
        const int gap = static_cast<int>(level) - static_cast<int>(e.level);
        if (gap <= 0 || e.severity >= std::min(5, 3 + gap)) out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Event& a, const Event& b) {
        return a.severity != b.severity ? a.severity > b.severity : a.time > b.time;
    });
    return out;
}

Assessment OsintEngine::assess(double latitude, double longitude, double radiusKm, std::int64_t now) const {
    std::lock_guard lock(mutex_);
    const std::int64_t t = clock(now);
    struct Weighted {
        double weight;
        double confidence;
        const Event* event;
    };
    std::vector<Weighted> hits;
    for (const auto& e : events_) {
        if (haversineMeters(latitude, longitude, e.latitude, e.longitude) > (radiusKm + e.radiusKm) * 1000.0) continue;
        hits.push_back({weightLocked(e, t), confidenceLocked(e), &e});
    }
    std::sort(hits.begin(), hits.end(), [](const Weighted& a, const Weighted& b) { return a.weight > b.weight; });

    Assessment a;
    bool firm = false;
    for (const auto& h : hits) {
        a.score += h.weight;
        a.events.push_back(*h.event);
        if (h.event->severity >= 5 && h.confidence >= 0.6 && h.weight >= 1.0) firm = true;
    }
    a.threat = a.score >= kScoreCritical || firm ? Threat::Critical
               : a.score >= kScoreElevated       ? Threat::Elevated
               : a.score >= kScoreAdvisory       ? Threat::Advisory
                                                 : Threat::Clear;
    std::ostringstream s;
    s << threatName(a.threat) << ": " << a.events.size() << " event(s) within " << radiusKm << " km, score " << std::round(a.score * 10.0) / 10.0;
    if (!a.events.empty()) s << ", led by \"" << a.events.front().title << "\"";
    a.summary = s.str();
    return a;
}

Reconciliation OsintEngine::reconcile(const Report& report, std::int64_t now) const {
    Reconciliation r;
    r.assessment = assess(report.latitude, report.longitude, report.radiusKm, now);
    const Threat t = r.assessment.threat;
    if (report.claimsSecure && t >= Threat::Elevated) {
        r.discrepancy = true;
        r.finding = "report" + (report.source.empty() ? std::string() : " from " + report.source) + " claims the sector is secure but independent evidence says " +
                    threatName(t) + ": " + r.assessment.summary;
    } else if (!report.claimsSecure && t == Threat::Clear) {
        r.discrepancy = true;
        r.finding = "report warns of a hostile sector but nothing corroborates it: " + r.assessment.summary;
    } else {
        r.finding = "report is consistent with the evidence: " + r.assessment.summary;
    }
    return r;
}

std::optional<Site> OsintEngine::nearestSite(double latitude, double longitude, double* distanceMeters) const {
    std::lock_guard lock(mutex_);
    std::optional<Site> best;
    double bestDistance = 0.0;
    for (const auto& s : sites_) {
        const double d = haversineMeters(latitude, longitude, s.latitude, s.longitude);
        if (!best || d < bestDistance) {
            best = s;
            bestDistance = d;
        }
    }
    if (best && distanceMeters != nullptr) *distanceMeters = bestDistance;
    return best;
}

namespace {

// 0.8..1 inside the site's radius, then a smooth fall-off outside it.
double proximity(double distance, double radius) {
    if (distance <= radius) return 1.0 - 0.2 * (distance / radius);
    return kVerifiedConfidence * std::exp(-(distance - radius) / (2.0 * radius));
}

} // namespace

GeolocationMatch OsintEngine::verifyLocation(const ImageMetadata& meta) const {
    GeolocationMatch m;
    if (!meta.hasGps()) {
        m.verificationNotes = "image carries no GPS position";
        return m;
    }
    m.latitude = *meta.latitude;
    m.longitude = *meta.longitude;

    std::lock_guard lock(mutex_);
    if (sites_.empty()) {
        m.verificationNotes = "no reference sites loaded";
        return m;
    }

    std::string text = lower(meta.description + " " + meta.iptcTitle + " " + meta.iptcCaption + " " + meta.iptcCity);
    for (const auto& k : meta.iptcKeywords) text += " " + lower(k);

    const Site* nearest = nullptr;
    double nearestDistance = 0.0;
    const Site* claimed = nullptr;
    for (const auto& s : sites_) {
        const double d = haversineMeters(m.latitude, m.longitude, s.latitude, s.longitude);
        if (nearest == nullptr || d < nearestDistance) {
            nearest = &s;
            nearestDistance = d;
        }
        if (text.find(lower(s.name)) != std::string::npos) claimed = &s;
    }

    std::ostringstream notes;
    const Site* matched = nearest;
    double distance = nearestDistance;
    double confidence = proximity(distance, nearest->radiusMeters) * 0.9; // GPS alone, nothing else to corroborate it

    if (claimed != nullptr) {
        const double claimedDistance = haversineMeters(m.latitude, m.longitude, claimed->latitude, claimed->longitude);
        if (claimedDistance > kMismatchRadiusFactor * claimed->radiusMeters) {
            matched = claimed;
            distance = claimedDistance;
            confidence = std::min(0.2, proximity(claimedDistance, claimed->radiusMeters));
            notes << "metadata names \"" << claimed->name << "\" but the GPS fix is " << std::llround(claimedDistance)
                  << " m away: mislabelled or spoofed. ";
        } else {
            matched = claimed;
            distance = claimedDistance;
            confidence = std::min(1.0, proximity(claimedDistance, claimed->radiusMeters) * 0.9 + 0.1);
            notes << "GPS fix and the image's own description agree on \"" << claimed->name << "\". ";
        }
    } else {
        notes << "GPS fix is " << std::llround(distance) << " m from \"" << matched->name << "\"; the image names no site to cross-check. ";
    }

    const std::string software = lower(meta.software);
    for (const char* editor : {"photoshop", "gimp", "lightroom", "affinity", "paint.net"}) {
        if (software.find(editor) != std::string::npos) {
            confidence = std::max(0.0, confidence - kEditedSoftwarePenalty);
            notes << "edited with " << meta.software << ", so metadata may have been rewritten. ";
            break;
        }
    }

    m.locationName = matched->name;
    m.deltaMeters = distance;
    m.confidence = std::clamp(confidence, 0.0, 1.0);
    m.verified = m.confidence >= kVerifiedConfidence;
    std::string out = notes.str();
    if (!out.empty()) out.pop_back();
    m.verificationNotes = out;
    return m;
}

GeolocationMatch OsintEngine::verifyLocation(const std::string& imagePath) const {
    ImageMetadata meta;
    std::string error;
    if (!parseImageFile(imagePath, meta, &error)) {
        GeolocationMatch m;
        m.verificationNotes = error;
        return m;
    }
    return verifyLocation(meta);
}

void OsintEngine::setOverlay(bool on) {
    std::lock_guard lock(mutex_);
    overlay_ = on;
}

bool OsintEngine::toggleOverlay() {
    std::lock_guard lock(mutex_);
    overlay_ = !overlay_;
    return overlay_;
}

Level OsintEngine::cycleLevel() {
    std::lock_guard lock(mutex_);
    overlayLevel_ = static_cast<Level>((static_cast<int>(overlayLevel_) + 1) % kLevelCount);
    return overlayLevel_;
}

bool OsintEngine::isOverlayActive() const {
    std::lock_guard lock(mutex_);
    return overlay_;
}

Level OsintEngine::overlayLevel() const {
    std::lock_guard lock(mutex_);
    return overlayLevel_;
}

std::vector<std::string> OsintEngine::overlayLines(std::size_t maxLines, std::int64_t now) const {
    const Level level = overlayLevel();
    const auto events = feed(level);
    const std::int64_t t = clock(now);
    std::vector<std::string> lines;
    lines.push_back(std::string("OSINT ") + levelName(level) + ": " + std::to_string(events.size()) + " event(s)");
    for (const auto& e : events) {
        if (lines.size() > maxLines) break;
        std::ostringstream s;
        const std::int64_t age = std::max<std::int64_t>(0, t - e.time);
        s << "[" << levelName(e.level) << "] " << domainName(e.domain) << " sev" << e.severity << " " << e.title << " ("
          << (age >= 86400 ? std::to_string(age / 86400) + "d" : age >= 3600 ? std::to_string(age / 3600) + "h" : std::to_string(age / 60) + "m") << " ago)";
        lines.push_back(s.str());
    }
    return lines;
}

json::Value OsintEngine::toJson(const Event& e) {
    json::Value j = json::Value::object();
    j["id"] = e.id;
    j["level"] = levelName(e.level);
    j["domain"] = domainName(e.domain);
    j["title"] = e.title;
    j["summary"] = e.summary;
    j["latitude"] = e.latitude;
    j["longitude"] = e.longitude;
    j["radius_km"] = e.radiusKm;
    j["severity"] = e.severity;
    j["source"] = e.source;
    j["time"] = e.time;
    j["confidence"] = e.confidence;
    return j;
}

json::Value OsintEngine::toJson(const Assessment& a) {
    json::Value j = json::Value::object();
    j["threat"] = threatName(a.threat);
    j["score"] = a.score;
    j["summary"] = a.summary;
    json::Value list = json::Value::array();
    for (const auto& e : a.events) list.push(toJson(e));
    j["events"] = std::move(list);
    return j;
}

json::Value OsintEngine::toJson(const GeolocationMatch& m) {
    json::Value j = json::Value::object();
    j["verified"] = m.verified;
    j["confidence"] = m.confidence;
    j["latitude"] = m.latitude;
    j["longitude"] = m.longitude;
    j["delta_m"] = m.deltaMeters;
    j["location"] = m.locationName;
    j["notes"] = m.verificationNotes;
    return j;
}

} // namespace gygax::intel
