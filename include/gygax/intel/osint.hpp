#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gygax/core/json.hpp>
#include <gygax/intel/metadata.hpp>

namespace gygax::intel {

// How far an event reaches. A monitor at one level sees everything at that level or wider, and lower-level
// events only when they are severe enough to climb (see OsintEngine::feed).
enum class Level { Local = 0, Regional, National, Theater, Global };
enum class Domain { Infrastructure, Maritime, Cyber, Aerospace, Geophysical, Security, Other };
enum class Threat { Clear, Advisory, Elevated, Critical };

constexpr int kLevelCount = 5;

const char* levelName(Level level);
const char* domainName(Domain domain);
const char* threatName(Threat threat);
std::optional<Level> parseLevel(std::string_view text);
std::optional<Domain> parseDomain(std::string_view text);

double haversineMeters(double lat1, double lon1, double lat2, double lon2);
// Seconds since the epoch from "YYYY-MM-DD[T ]HH:MM:SS[Z]" (UTC); nullopt when malformed.
std::optional<std::int64_t> parseIsoTime(std::string_view text);

struct Event {
    std::string id;
    Level level = Level::Local;
    Domain domain = Domain::Other;
    std::string title;
    std::string summary;
    double latitude = 0.0;
    double longitude = 0.0;
    double radiusKm = 0.0;
    int severity = 1; // 1 (routine) to 5 (critical)
    std::string source;
    std::int64_t time = 0;   // seconds since the epoch
    double confidence = 0.5; // the source's own confidence, 0..1
};

struct Site {
    std::string name;
    double latitude = 0.0;
    double longitude = 0.0;
    double radiusMeters = 150.0; // how close a photograph must be to count as "at" the site
};

struct Assessment {
    Threat threat = Threat::Clear;
    double score = 0.0;
    std::vector<Event> events; // contributing events, highest weight first
    std::string summary;
};

struct Report {
    double latitude = 0.0;
    double longitude = 0.0;
    double radiusKm = 5.0;
    bool claimsSecure = true;
    std::string source;
};

struct Reconciliation {
    bool discrepancy = false;
    Assessment assessment;
    std::string finding;
};

struct GeolocationMatch {
    bool verified = false;
    double confidence = 0.0;
    double latitude = 0.0;
    double longitude = 0.0;
    double deltaMeters = 0.0; // distance from the photograph's GPS fix to the matched site
    std::string locationName;
    std::string verificationNotes;
};

struct FeedOptions {
    std::int64_t now = 0;           // seconds; 0 means the wall clock
    double halfLifeHours = 6.0;     // how quickly an event stops mattering
    double corroborationKm = 25.0;  // reports this close and in time...
    double corroborationHours = 24; // ...from different sources support each other
};

class OsintEngine {
public:
    OsintEngine();
    explicit OsintEngine(FeedOptions options);

    void addEvent(Event event);
    void addSite(Site site);
    // Accepts {"events":[...]}, a bare array of events, or a GeoJSON FeatureCollection of points (such as the
    // USGS earthquake feeds). Returns the number of events taken; malformed entries are skipped and counted
    // in *skipped.
    std::size_t loadJson(const json::Value& feed, std::string* error = nullptr, std::size_t* skipped = nullptr);
    std::size_t loadFile(const std::string& path, std::string* error = nullptr);
    // Plain HTTP GET of a JSON feed.
    std::size_t fetch(const std::string& url, std::string* error = nullptr);
    std::size_t loadSites(const json::Value& sites);

    [[nodiscard]] std::size_t eventCount() const;
    [[nodiscard]] std::vector<Event> feed(Level level, std::optional<Domain> domain = std::nullopt) const;
    // Probability-style support for an event: its own confidence combined with independent corroboration.
    [[nodiscard]] double confidenceOf(const Event& event) const;
    [[nodiscard]] Assessment assess(double latitude, double longitude, double radiusKm, std::int64_t now = 0) const;
    [[nodiscard]] Reconciliation reconcile(const Report& report, std::int64_t now = 0) const;

    [[nodiscard]] std::optional<Site> nearestSite(double latitude, double longitude, double* distanceMeters = nullptr) const;
    [[nodiscard]] GeolocationMatch verifyLocation(const ImageMetadata& meta) const;
    [[nodiscard]] GeolocationMatch verifyLocation(const std::string& imagePath) const;

    // Operator HUD state: O toggles the overlay, L cycles the level it shows.
    void setOverlay(bool on);
    bool toggleOverlay();
    Level cycleLevel();
    [[nodiscard]] bool isOverlayActive() const;
    [[nodiscard]] Level overlayLevel() const;
    [[nodiscard]] std::vector<std::string> overlayLines(std::size_t maxLines = 8, std::int64_t now = 0) const;

    [[nodiscard]] static json::Value toJson(const Event& event);
    [[nodiscard]] static json::Value toJson(const Assessment& assessment);
    [[nodiscard]] static json::Value toJson(const GeolocationMatch& match);

private:
    [[nodiscard]] std::int64_t clock(std::int64_t now) const;
    [[nodiscard]] double confidenceLocked(const Event& event) const;
    [[nodiscard]] double weightLocked(const Event& event, std::int64_t now) const;

    FeedOptions options_;
    mutable std::mutex mutex_;
    std::vector<Event> events_;
    std::vector<Site> sites_;
    bool overlay_ = true;
    Level overlayLevel_ = Level::Local;
};

} // namespace gygax::intel
