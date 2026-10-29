#include <gygax/oliphaunt/memory.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

#include <gygax/intel/osint.hpp>

namespace gygax::oliphaunt {

namespace {

constexpr std::int64_t kDay = 86400;

std::int64_t wallClock() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// FIPS 180-4 SHA-256.
std::string sha256Hex(const std::string& message) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string data = message;
    const std::uint64_t bits = static_cast<std::uint64_t>(message.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56) data.push_back('\0');
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
    auto rotr = [](std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t off = 0; off < data.size(); off += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(data.data() + off + i * 4);
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto v = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
            const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const std::uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
            const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const std::uint32_t t2 = s0 + maj;
            v[7] = v[6];
            v[6] = v[5];
            v[5] = v[4];
            v[4] = v[3] + t1;
            v[3] = v[2];
            v[2] = v[1];
            v[1] = v[0];
            v[0] = t1 + t2;
        }
        for (std::size_t i = 0; i < 8; ++i) h[i] += v[i];
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (std::uint32_t word : h)
        for (int shift = 28; shift >= 0; shift -= 4) out.push_back(hex[(word >> shift) & 0xF]);
    return out;
}

std::string normalise(Kind kind, const std::string& identifier) {
    std::string out;
    for (char ch : identifier) {
        const auto c = static_cast<unsigned char>(ch);
        if (kind == Kind::Vehicle) {
            if (std::isalnum(c)) out.push_back(static_cast<char>(std::toupper(c)));
        } else if (!std::isspace(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ago(std::int64_t seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds < 90) return "just now";
    if (seconds < 90 * 60) return std::to_string(seconds / 60) + " min ago";
    if (seconds < 36 * 3600) return std::to_string(seconds / 3600) + " h ago";
    return std::to_string(seconds / kDay) + " days ago";
}

std::string randomSalt() {
    std::random_device rd;
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 32; ++i) out.push_back(hex[rd() & 0xF]);
    return out;
}

} // namespace

const char* kindName(Kind kind) {
    switch (kind) {
    case Kind::Vehicle: return "vehicle";
    case Kind::Person: return "person";
    case Kind::Device: return "device";
    case Kind::Other: break;
    }
    return "other";
}

const char* concernName(Concern concern) {
    switch (concern) {
    case Concern::None: return "none";
    case Concern::Noted: return "noted";
    case Concern::Notice: return "notice";
    case Concern::Alert: return "alert";
    case Concern::Persistent: break;
    }
    return "persistent";
}

std::optional<Kind> parseKind(std::string_view text) {
    const std::string t = lower(std::string(text));
    if (t == "vehicle" || t == "car" || t == "plate") return Kind::Vehicle;
    if (t == "person" || t == "face") return Kind::Person;
    if (t == "device" || t == "mac" || t == "radio") return Kind::Device;
    if (t == "other") return Kind::Other;
    return std::nullopt;
}

std::optional<Concern> parseConcern(std::string_view text) {
    const std::string t = lower(std::string(text));
    for (Concern c : {Concern::None, Concern::Noted, Concern::Notice, Concern::Alert, Concern::Persistent})
        if (t == concernName(c)) return c;
    return std::nullopt;
}

Memory::Memory(std::string path, Options options) : path_(std::move(path)), options_(options) {
    if (!path_.empty()) load();
    if (salt_.empty()) salt_ = randomSalt();
}

std::string Memory::hashOf(Kind kind, const std::string& identifier) const {
    return sha256Hex(salt_ + "|" + kindName(kind) + "|" + normalise(kind, identifier));
}

const Memory::Entity* Memory::find(const std::string& hash) const {
    for (const auto& e : entities_)
        if (e.hash == hash) return &e;
    return nullptr;
}

bool Memory::inSafePlace(const Encounter& e) const {
    for (const auto& p : safe_) {
        if (e.hasFix) {
            if (intel::haversineMeters(e.latitude, e.longitude, p.latitude, p.longitude) <= p.radiusMeters) return true;
        } else if (!e.place.empty() && lower(e.place) == lower(p.name)) {
            return true;
        }
    }
    return false;
}

Finding Memory::assess(const Entity& entity, std::int64_t now) const {
    Finding f;
    f.id = entity.hash.substr(0, 12);
    f.kind = entity.kind;
    f.label = entity.label;
    f.trusted = entity.trusted;

    std::vector<Encounter> sorted = entity.encounters;
    std::sort(sorted.begin(), sorted.end(), [](const Encounter& a, const Encounter& b) { return a.time < b.time; });

    struct Cluster {
        bool hasFix;
        double lat, lon;
        std::string name;
    };
    std::vector<Cluster> clusters;
    std::set<std::int64_t> days;
    std::int64_t lastCounted = 0, firstAway = 0, lastAway = 0;
    bool any = false;
    for (const auto& e : sorted) {
        if (any && e.time - lastCounted < options_.encounterGapSeconds) continue;
        any = true;
        lastCounted = e.time;
        ++f.sightings;
        if (f.firstSeen == 0) f.firstSeen = e.time;
        f.lastSeen = e.time;
        if (inSafePlace(e)) continue;
        if (firstAway == 0) firstAway = e.time;
        lastAway = e.time;
        days.insert(e.time / kDay);
        bool known = false;
        for (const auto& c : clusters) {
            if (e.hasFix && c.hasFix) {
                known = intel::haversineMeters(e.latitude, e.longitude, c.lat, c.lon) < options_.placeSeparationMeters;
            } else if (!e.hasFix && !c.hasFix) {
                known = !e.place.empty() && lower(e.place) == c.name;
            }
            if (known) break;
        }
        if (!known) clusters.push_back({e.hasFix, e.latitude, e.longitude, lower(e.place)});
    }
    f.places = static_cast<int>(clusters.size());
    f.days = static_cast<int>(days.size());
    f.spanDays = firstAway ? static_cast<double>(lastAway - firstAway) / kDay : 0.0;
    f.score = f.places >= 2 ? std::min(f.places, 8) + 0.5 * std::min(f.days, 14) : 0.25 * std::min(f.days, 6);

    if (f.sightings >= 2) f.concern = Concern::Noted;
    if (f.places >= 3 && f.days >= 2 && f.score >= options_.noticeScore) f.concern = Concern::Notice;
    if (f.places >= 3 && f.days >= 3 && f.score >= options_.alertScore) f.concern = Concern::Alert;
    if (f.places >= 3 && f.days >= options_.persistentDays && f.spanDays >= options_.persistentDays) f.concern = Concern::Persistent;
    if (entity.trusted) f.concern = Concern::None;

    std::ostringstream s;
    s << (entity.label.empty() ? kindName(entity.kind) : entity.label) << " [" << f.id << "]: ";
    if (entity.trusted) {
        s << "trusted, seen " << f.sightings << " times";
    } else {
        s << "seen " << f.sightings << (f.sightings == 1 ? " time" : " times");
        if (f.places > 0) s << " at " << f.places << (f.places == 1 ? " place" : " places") << " away from your safe places";
        if (f.days > 1) s << " over " << f.days << " days (" << static_cast<int>(f.spanDays) << " day span)";
    }
    s << "; last " << ago((now ? now : wallClock()) - f.lastSeen);
    f.summary = s.str();
    return f;
}

std::optional<Finding> Memory::observe(const Sighting& s, std::string* error) {
    auto fail = [&](const char* why) -> std::optional<Finding> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (normalise(s.kind, s.identifier).empty()) return fail("identifier is empty");
    if (s.latitude.has_value() != s.longitude.has_value()) return fail("latitude and longitude go together");
    if (s.latitude && (!std::isfinite(*s.latitude) || !std::isfinite(*s.longitude) || std::abs(*s.latitude) > 90 ||
                       std::abs(*s.longitude) > 180))
        return fail("position out of range");
    if (s.time < 0) return fail("time is negative");

    const std::int64_t now = wallClock();
    const std::int64_t when = s.time ? s.time : now;
    if (options_.retentionDays > 0 && when < now - static_cast<std::int64_t>(options_.retentionDays) * kDay)
        return fail("sighting is older than the retention period");
    std::lock_guard lock(mutex_);
    const std::string hash = hashOf(s.kind, s.identifier);
    Entity* entity = nullptr;
    for (auto& e : entities_)
        if (e.hash == hash) entity = &e;
    if (!entity) {
        if (entities_.size() >= options_.maxEntities) {
            auto oldest = std::min_element(entities_.begin(), entities_.end(), [](const Entity& a, const Entity& b) {
                if (a.trusted != b.trusted) return !a.trusted;
                const std::int64_t ta = a.encounters.empty() ? 0 : a.encounters.back().time;
                const std::int64_t tb = b.encounters.empty() ? 0 : b.encounters.back().time;
                return ta < tb;
            });
            entities_.erase(oldest);
        }
        entities_.push_back({hash, s.kind, {}, false, {}});
        entity = &entities_.back();
    }
    if (!s.label.empty()) entity->label = s.label.substr(0, 80);

    Encounter enc;
    enc.time = when;
    enc.hasFix = s.latitude.has_value();
    if (enc.hasFix) {
        enc.latitude = *s.latitude;
        enc.longitude = *s.longitude;
    }
    enc.place = s.place.substr(0, 80);
    // A continuous camera feed would otherwise bury the history under copies of one encounter.
    const bool repeat = !entity->encounters.empty() && std::llabs(when - entity->encounters.back().time) < options_.encounterGapSeconds &&
                        (enc.hasFix == entity->encounters.back().hasFix) &&
                        (!enc.hasFix || intel::haversineMeters(enc.latitude, enc.longitude, entity->encounters.back().latitude,
                                                               entity->encounters.back().longitude) < options_.placeSeparationMeters);
    if (!repeat) {
        entity->encounters.push_back(enc);
        if (entity->encounters.size() > options_.maxSightingsPerEntity)
            entity->encounters.erase(entity->encounters.begin(),
                                     entity->encounters.begin() + static_cast<std::ptrdiff_t>(entity->encounters.size() - options_.maxSightingsPerEntity));
    }
    pruneLocked(now);
    const Entity* stored = find(hash);
    if (!stored) return fail("sighting was pruned");
    const Finding f = assess(*stored, std::max(now, when));
    saveLocked();
    return f;
}

std::optional<Finding> Memory::lookup(Kind kind, const std::string& identifier, std::int64_t now) const {
    std::lock_guard lock(mutex_);
    const Entity* e = find(hashOf(kind, identifier));
    if (!e) return std::nullopt;
    return assess(*e, now);
}

std::vector<Finding> Memory::report(Concern minimum, std::int64_t now) const {
    std::lock_guard lock(mutex_);
    std::vector<Finding> out;
    for (const auto& e : entities_) {
        Finding f = assess(e, now);
        if (f.concern >= minimum) out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) {
        if (a.concern != b.concern) return a.concern > b.concern;
        return a.score > b.score;
    });
    return out;
}

bool Memory::trust(Kind kind, const std::string& identifier, bool trusted) {
    std::lock_guard lock(mutex_);
    const std::string hash = hashOf(kind, identifier);
    for (auto& e : entities_)
        if (e.hash == hash) {
            e.trusted = trusted;
            saveLocked();
            return true;
        }
    if (!trusted) return false;
    entities_.push_back({hash, kind, {}, true, {}});
    saveLocked();
    return true;
}

void Memory::addSafePlace(Place place) {
    std::lock_guard lock(mutex_);
    safe_.push_back(std::move(place));
    saveLocked();
}

std::vector<Place> Memory::safePlaces() const {
    std::lock_guard lock(mutex_);
    return safe_;
}

bool Memory::forget(Kind kind, const std::string& identifier) {
    std::lock_guard lock(mutex_);
    const std::string hash = hashOf(kind, identifier);
    const auto it = std::remove_if(entities_.begin(), entities_.end(), [&](const Entity& e) { return e.hash == hash; });
    const bool removed = it != entities_.end();
    entities_.erase(it, entities_.end());
    if (removed) saveLocked();
    return removed;
}

bool Memory::trustById(const std::string& id, bool trusted) {
    if (id.size() < 6) return false;
    std::lock_guard lock(mutex_);
    Entity* match = nullptr;
    for (auto& e : entities_)
        if (e.hash.compare(0, id.size(), id) == 0) {
            if (match) return false; // ambiguous
            match = &e;
        }
    if (!match) return false;
    match->trusted = trusted;
    saveLocked();
    return true;
}

bool Memory::forgetById(const std::string& id) {
    if (id.size() < 6) return false;
    std::lock_guard lock(mutex_);
    const Entity* match = nullptr;
    for (const auto& e : entities_)
        if (e.hash.compare(0, id.size(), id) == 0) {
            if (match) return false;
            match = &e;
        }
    if (!match) return false;
    const std::string hash = match->hash;
    std::erase_if(entities_, [&](const Entity& e) { return e.hash == hash; });
    saveLocked();
    return true;
}

void Memory::forgetAll() {
    std::lock_guard lock(mutex_);
    entities_.clear();
    safe_.clear();
    salt_ = randomSalt();
    saveLocked();
}

std::size_t Memory::pruneLocked(std::int64_t now) {
    if (options_.retentionDays <= 0) return 0;
    const std::int64_t cutoff = (now ? now : wallClock()) - static_cast<std::int64_t>(options_.retentionDays) * kDay;
    std::size_t dropped = 0;
    for (auto& e : entities_) {
        const auto before = e.encounters.size();
        std::erase_if(e.encounters, [&](const Encounter& x) { return x.time < cutoff; });
        dropped += before - e.encounters.size();
    }
    // A trusted entity with no history is kept: the trust is the information.
    std::erase_if(entities_, [](const Entity& e) { return e.encounters.empty() && !e.trusted; });
    return dropped;
}

std::size_t Memory::prune(std::int64_t now) {
    std::lock_guard lock(mutex_);
    const std::size_t n = pruneLocked(now);
    if (n) saveLocked();
    return n;
}

std::size_t Memory::entityCount() const {
    std::lock_guard lock(mutex_);
    return entities_.size();
}

std::string Memory::lastError() const {
    std::lock_guard lock(mutex_);
    return error_;
}

json::Value Memory::toJson(const Finding& f) {
    json::Value v = json::Value::object();
    v["id"] = f.id;
    v["kind"] = kindName(f.kind);
    v["label"] = f.label;
    v["concern"] = concernName(f.concern);
    v["sightings"] = f.sightings;
    v["places"] = f.places;
    v["days"] = f.days;
    v["span_days"] = f.spanDays;
    v["first_seen"] = f.firstSeen;
    v["last_seen"] = f.lastSeen;
    v["score"] = f.score;
    v["trusted"] = f.trusted;
    v["summary"] = f.summary;
    return v;
}

void Memory::saveLocked() {
    if (path_.empty()) return;
    json::Value root = json::Value::object();
    root["version"] = 1;
    root["salt"] = salt_;
    json::Value safe = json::Value::array();
    for (const auto& p : safe_) {
        json::Value o = json::Value::object();
        o["name"] = p.name;
        o["lat"] = p.latitude;
        o["lon"] = p.longitude;
        o["radius_m"] = p.radiusMeters;
        safe.push(std::move(o));
    }
    root["safe_places"] = std::move(safe);
    json::Value ents = json::Value::array();
    for (const auto& e : entities_) {
        json::Value o = json::Value::object();
        o["hash"] = e.hash;
        o["kind"] = kindName(e.kind);
        o["label"] = e.label;
        o["trusted"] = e.trusted;
        json::Value encs = json::Value::array();
        for (const auto& x : e.encounters) {
            json::Value item = json::Value::object();
            item["t"] = x.time;
            if (x.hasFix) {
                item["lat"] = x.latitude;
                item["lon"] = x.longitude;
            }
            if (!x.place.empty()) item["p"] = x.place;
            encs.push(std::move(item));
        }
        o["encounters"] = std::move(encs);
        ents.push(std::move(o));
    }
    root["entities"] = std::move(ents);

    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error_ = "cannot write " + tmp;
            return;
        }
        ::chmod(tmp.c_str(), 0600);
        out << root.dump();
        out.flush();
        if (!out) {
            error_ = "write failed for " + tmp;
            return;
        }
    }
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        error_ = "cannot replace " + path_;
        return;
    }
    error_.clear();
}

void Memory::load() {
    std::ifstream in(path_, std::ios::binary);
    if (!in) return; // a new memory
    std::stringstream buf;
    buf << in.rdbuf();
    std::string err;
    auto root = json::parse(buf.str(), &err);
    if (!root || !root->isObject() || root->getString("salt").empty()) {
        error_ = "memory file is unreadable: " + path_ + (err.empty() ? "" : " (" + err + ")");
        // Starting over would silently shadow years of history; keep the file and refuse to overwrite it.
        std::rename(path_.c_str(), (path_ + ".corrupt").c_str());
        return;
    }
    salt_ = root->getString("salt");
    if (const auto* safe = root->find("safe_places"))
        for (const auto& p : safe->asArray()) safe_.push_back({p.getString("name"), p.getDouble("lat"), p.getDouble("lon"), p.getDouble("radius_m", 150.0)});
    if (const auto* ents = root->find("entities"))
        for (const auto& o : ents->asArray()) {
            Entity e;
            e.hash = o.getString("hash");
            e.kind = parseKind(o.getString("kind")).value_or(Kind::Other);
            e.label = o.getString("label");
            e.trusted = o.find("trusted") && o.find("trusted")->asBool();
            if (const auto* encs = o.find("encounters"))
                for (const auto& x : encs->asArray()) {
                    Encounter enc;
                    enc.time = x.getInt("t");
                    enc.hasFix = x.find("lat") && x.find("lon");
                    if (enc.hasFix) {
                        enc.latitude = x.getDouble("lat");
                        enc.longitude = x.getDouble("lon");
                    }
                    enc.place = x.getString("p");
                    e.encounters.push_back(std::move(enc));
                }
            if (!e.hash.empty()) entities_.push_back(std::move(e));
        }
    std::lock_guard lock(mutex_);
    pruneLocked(wallClock());
}

} // namespace gygax::oliphaunt
