#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gygax/core/json.hpp>

namespace gygax::logistics {

using Attributes = std::map<std::string, std::string>;

struct Outcome {
    bool ok = true;
    int status = 200;
    std::string code;
    std::string message;
    json::Value body = json::Value::object();
};

struct ParsedLine {
    std::int64_t delta = 0;
    std::string sku;
    Attributes attrs;
    std::vector<std::string> guids;
    std::string reason;
    std::string site;
};

struct Point {
    double latitude = 0.0;
    double longitude = 0.0;
};

struct Model {
    std::string sku;
    std::string kind;
    std::string power;
    double maxRangeM = 0.0;
    double cruiseMps = 0.0;
    Attributes attrs;
};

struct Site {
    std::string id;
    std::string name;
    std::string kind;
    Point position;
    std::vector<std::string> services;
};

bool withinRange(double value, double low, double high);
std::string newGuid();
bool validGuid(std::string_view text);
bool validToken(std::string_view text);
bool parseLine(std::string_view line, ParsedLine& out, std::string* error);
std::string formatLine(std::int64_t delta, const std::string& sku, const Attributes& attrs);
std::optional<std::int64_t> parseSince(const json::Value& value, std::int64_t nowMs);
double distanceMeters(const Point& a, const Point& b);
Point pointToward(const Point& from, const Point& to, double meters);

struct PlanRequest {
    Point origin;
    std::vector<Point> waypoints;
    std::optional<Point> returnTo;
    double maxRangeM = 0.0;
    double fuelFraction = 1.0;
    double reserveFraction = 0.1;
    double cruiseMps = 0.0;
    std::string power;
};

json::Value planRoute(const PlanRequest& request, const std::vector<Site>& sites);

class Ledger {
public:
    explicit Ledger(std::string journalPath = {});
    ~Ledger();
    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;

    Outcome putModel(const json::Value& spec);
    Outcome putSite(const json::Value& spec);
    Outcome removeSite(const std::string& id);
    Outcome record(const json::Value& request);
    Outcome updateUnit(const std::string& guid, const json::Value& patch);

    [[nodiscard]] json::Value models() const;
    [[nodiscard]] json::Value sitesJson() const;
    [[nodiscard]] std::vector<Site> sites() const;
    [[nodiscard]] std::optional<Model> model(const std::string& sku) const;
    [[nodiscard]] json::Value events(const std::string& sku, std::int64_t sinceMs, std::size_t limit) const;
    [[nodiscard]] json::Value units(const std::string& sku, const std::string& status, std::size_t limit) const;
    [[nodiscard]] std::optional<json::Value> unit(const std::string& guid) const;
    [[nodiscard]] json::Value summary(std::int64_t sinceMs) const;

    struct Gauge {
        std::string sku;
        std::string status;
        std::uint64_t count;
    };
    [[nodiscard]] std::vector<Gauge> gauges() const;
    [[nodiscard]] std::uint64_t eventCount() const;
    [[nodiscard]] const std::string& journalPath() const { return journalPath_; }

    static constexpr std::size_t kMaxUnits = 1000000;
    static constexpr std::int64_t kMaxDelta = 100000;
    static constexpr std::size_t kMaxSites = 256;
    static constexpr std::size_t kMaxModels = 4096;

private:
    struct Unit {
        std::string guid;
        std::string sku;
        std::string status;
        Attributes attrs;
        std::string device;
        std::string site;
        double fuel = 1.0;
        std::optional<Point> position;
        std::int64_t createdMs = 0;
        std::int64_t endedMs = 0;
        std::string endReason;
    };
    struct Event {
        std::string id;
        std::int64_t timeMs = 0;
        std::string sku;
        std::int64_t delta = 0;
        std::vector<std::string> guids;
        std::string reason;
        std::string site;
        std::string note;
        Attributes attrs;
    };

    Outcome recordLocked(const json::Value& request, bool replay);
    void applyEvent(const Event& e);
    void journal(const json::Value& entry);
    void replay();
    static json::Value toJson(const Unit& u);
    static json::Value toJson(const Event& e);
    static json::Value toJson(const Model& m);
    static json::Value toJson(const Site& s);
    static bool matches(const Attributes& have, const Attributes& want);

    std::string journalPath_;
    void* journalFile_ = nullptr;
    mutable std::mutex mutex_;
    std::map<std::string, Model> models_;
    std::map<std::string, Site> sites_;
    std::map<std::string, Unit> units_;
    std::vector<Event> events_;
    bool replaying_ = false;
};

}
