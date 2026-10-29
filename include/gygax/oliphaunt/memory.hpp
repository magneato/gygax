#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gygax/core/json.hpp>

// Operation Oliphaunt: a memory for the things a person would forget. Sightings of vehicles, people and
// radio devices are stored against a salted hash of their identifier (a plate is never kept), and the
// memory reports anything that keeps turning up in places it has no business being.
namespace gygax::oliphaunt {

enum class Kind { Vehicle, Person, Device, Other };
enum class Concern { None, Noted, Notice, Alert, Persistent };

const char* kindName(Kind kind);
const char* concernName(Concern concern);
std::optional<Kind> parseKind(std::string_view text);
std::optional<Concern> parseConcern(std::string_view text);

struct Sighting {
    Kind kind = Kind::Vehicle;
    std::string identifier; // plate, face-cluster id, MAC...: hashed on arrival, never stored
    std::string label;      // what a human would call it ("grey hatchback"); keep it non-identifying
    std::optional<double> latitude;
    std::optional<double> longitude;
    std::string place;      // a named place when there is no fix (room, SSID, street)
    std::int64_t time = 0;  // seconds since the epoch; 0 means now
};

struct Place {
    std::string name;
    double latitude = 0.0;
    double longitude = 0.0;
    double radiusMeters = 150.0;
};

struct Finding {
    std::string id; // first 12 hex characters of the identifier hash
    Kind kind = Kind::Vehicle;
    std::string label;
    Concern concern = Concern::None;
    int sightings = 0;
    int places = 0; // distinct places away from the safe places
    int days = 0;   // distinct calendar days
    double spanDays = 0.0;
    std::int64_t firstSeen = 0;
    std::int64_t lastSeen = 0;
    double score = 0.0;
    bool trusted = false;
    std::string summary;
};

struct Options {
    double placeSeparationMeters = 400.0;   // fixes closer than this are the same place
    std::int64_t encounterGapSeconds = 600; // sightings closer in time than this are one encounter
    int retentionDays = 180;                // older sightings are dropped; 0 keeps everything
    std::size_t maxSightingsPerEntity = 500;
    std::size_t maxEntities = 20000;
    double noticeScore = 3.0;
    double alertScore = 6.0;
    int persistentDays = 7; // "for weeks": seen across this many days at 3+ places
};

class Memory {
public:
    // An empty path keeps the memory in RAM only. Otherwise it is loaded from, and written back to, that file
    // (mode 0600) after each change.
    explicit Memory(std::string path = {}, Options options = {});

    // Returns the entity's finding after the sighting was added, or nullopt if the sighting was unusable
    // (empty identifier, or a position outside the valid range).
    std::optional<Finding> observe(const Sighting& sighting, std::string* error = nullptr);
    [[nodiscard]] std::optional<Finding> lookup(Kind kind, const std::string& identifier, std::int64_t now = 0) const;
    // Everything that has reached at least `minimum`, most concerning first.
    [[nodiscard]] std::vector<Finding> report(Concern minimum = Concern::Notice, std::int64_t now = 0) const;

    // Neighbours, colleagues and your own car: never alerted on.
    bool trust(Kind kind, const std::string& identifier, bool trusted = true);
    // Sightings inside a safe place do not count towards "followed": what matters is meeting it elsewhere.
    void addSafePlace(Place place);
    [[nodiscard]] std::vector<Place> safePlaces() const;

    bool forget(Kind kind, const std::string& identifier);
    // By the id a Finding carries (its hash prefix, at least 6 characters, unambiguous): for headsets and UIs that
    // never see the identifier.
    bool trustById(const std::string& id, bool trusted = true);
    bool forgetById(const std::string& id);
    void forgetAll();
    std::size_t prune(std::int64_t now = 0);

    [[nodiscard]] std::size_t entityCount() const;
    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] std::string lastError() const;

    [[nodiscard]] static json::Value toJson(const Finding& finding);

private:
    struct Encounter {
        std::int64_t time = 0;
        bool hasFix = false;
        double latitude = 0.0;
        double longitude = 0.0;
        std::string place;
    };
    struct Entity {
        std::string hash; // hex SHA-256 of salt + kind + normalised identifier
        Kind kind = Kind::Vehicle;
        std::string label;
        bool trusted = false;
        std::vector<Encounter> encounters;
    };

    [[nodiscard]] std::string hashOf(Kind kind, const std::string& identifier) const;
    [[nodiscard]] Finding assess(const Entity& entity, std::int64_t now) const;
    [[nodiscard]] bool inSafePlace(const Encounter& e) const;
    [[nodiscard]] const Entity* find(const std::string& hash) const;
    std::size_t pruneLocked(std::int64_t now);
    void load();
    void saveLocked();

    std::string path_;
    Options options_;
    mutable std::mutex mutex_;
    std::string salt_;
    std::vector<Entity> entities_;
    std::vector<Place> safe_;
    std::string error_;
};

} // namespace gygax::oliphaunt
