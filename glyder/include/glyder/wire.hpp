#pragma once

// The glyder protocol: how a Gygax simulation (the host) shows its world to glyder clients and
// takes a pilot's controls back, over UDP on a LAN.
//
// Latency first. World state goes out as self-contained snapshot packets, 34 bytes per entity, with
// velocity, so a lost packet costs nothing and the client can interpolate or extrapolate between
// them; the newest packet always wins. Head motion never waits on the network: it is rendered
// locally from the headset's own tracking. A pilot's controls go up as unreliable packets at the
// display rate, newest wins. The few things that must arrive (drop-in requests and grants, orders,
// narrative events, terrain) use a small acknowledged channel with in-order delivery.
//
// Every packet after the handshake carries an 8-byte HMAC-SHA-256 tag under a session key derived
// from the pairing secret and both sides' nonces, so nobody else on the LAN can drive a node or
// forge a grant. Packets are at most 1200 bytes so they are never fragmented.
//
// All integers little-endian. Positions are metres in the host's ENU frame (x east, y north,
// z up) relative to the world origin in Welcome.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glyder/math.hpp>

namespace glyder::wire {

inline constexpr std::uint32_t kMagic = 0x31594C47u; // "GLY1"
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::uint16_t kDefaultPort = 1999;
inline constexpr std::size_t kMaxPacket = 1200;
inline constexpr std::size_t kHeaderBytes = 16;
inline constexpr std::size_t kTagBytes = 8;
inline constexpr std::size_t kMaxPayload = kMaxPacket - kHeaderBytes - kTagBytes;

enum class Type : std::uint8_t {
    Discover = 1,   // client, broadcast: who is there?
    Beacon = 2,     // host: name, title, pairing needed
    Hello = 3,      // client: name, nonce
    Challenge = 4,  // host: nonce
    Auth = 5,       // client: proof of the pairing secret
    Welcome = 6,    // host: session and world
    Snapshot = 7,   // host: entity states (unreliable)
    Effects = 8,    // host: lasers, tracers, rings for this tick (unreliable)
    Info = 9,       // host: entity names and tasks (unreliable, round robin)
    Status = 10,    // host: HUD readouts (unreliable)
    Event = 11,     // host: narrative line (reliable)
    DropIn = 12,    // client: ride an entity (reliable)
    DropState = 13, // host: riding, control, denied, released (reliable)
    Control = 14,   // client: stick and buttons (unreliable)
    Release = 15,   // client: give control back (reliable)
    Order = 16,     // client: a directive, as from gyde (reliable)
    Ping = 17,
    Pong = 18,
    Ack = 19,
    Bye = 20,
    Terrain = 21, // host: a chunk of the heightfield (reliable)
    Denied = 22,  // host: pairing failed or session unknown
};

enum Flag : std::uint8_t { kReliable = 1, kTagged = 2 };

struct Header {
    Type type{Type::Ping};
    std::uint8_t flags{0};
    std::uint32_t session{0};
    std::uint32_t seq{0}; // reliable sequence number, or a packet counter
};

// ---------------------------------------------------------------------------------------------
// Byte codec

class Writer {
public:
    void u8(std::uint8_t v) { b_.push_back(v); }
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
    void f32(float v);
    void f64(double v);
    void str8(std::string_view s);  // up to 255 bytes
    void str16(std::string_view s); // up to 65535 bytes
    void bytes(std::span<const std::uint8_t> s) { b_.insert(b_.end(), s.begin(), s.end()); }
    [[nodiscard]] const std::vector<std::uint8_t>& data() const { return b_; }
    [[nodiscard]] std::vector<std::uint8_t>& data() { return b_; }
    [[nodiscard]] std::size_t size() const { return b_.size(); }

private:
    std::vector<std::uint8_t> b_;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> s) : s_(s) {}
    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    float f32();
    double f64();
    std::string str8();
    std::string str16();
    std::span<const std::uint8_t> bytes(std::size_t n);
    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::size_t remaining() const { return ok_ ? s_.size() - p_ : 0; }

private:
    bool need(std::size_t n);
    std::span<const std::uint8_t> s_;
    std::size_t p_{0};
    bool ok_{true};
};

// A quaternion in 32 bits: the index of the largest component and the other three at 10 bits each.
[[nodiscard]] std::uint32_t packQuat(Quat q);
[[nodiscard]] Quat unpackQuat(std::uint32_t v);

// ---------------------------------------------------------------------------------------------
// Packets: header + payload + optional tag.

void writeHeader(Writer& w, const Header& h);
[[nodiscard]] std::optional<Header> readHeader(Reader& r);
// Append the tag over everything written so far.
void sign(std::vector<std::uint8_t>& packet, std::span<const std::uint8_t> key);
// Verify and strip the tag; false if it does not match.
[[nodiscard]] bool verify(std::span<const std::uint8_t> packet, std::span<const std::uint8_t> key);

// ---------------------------------------------------------------------------------------------
// Messages

struct Beacon {
    std::string name, title;
    std::uint16_t port{kDefaultPort};
    std::uint16_t entities{0};
    bool pairing{false};
    std::uint8_t environment{0};
};

struct Hello {
    std::string client;
    std::array<std::uint8_t, 16> nonce{};
};

struct Challenge {
    std::array<std::uint8_t, 16> nonce{};
    bool pairing{false};
};

struct Auth {
    std::array<std::uint8_t, 32> proof{};
};

struct World {
    std::uint32_t session{0};
    double hostTime{0};
    std::uint16_t snapshotHz{30};
    std::uint8_t environment{0}; // render::Environment
    std::string title, subtitle;
    double originLat{0}, originLon{0};
    float groundZ{0};    // mean ground height (m)
    float radius{10000}; // the world's extent from the origin, for framing
    std::uint32_t terrainBytes{0};
    std::string help; // what the host lets a pilot do
};

enum EntityFlag : std::uint8_t {
    kAlive = 1,
    kDrivable = 2, // a pilot may drop in
    kPhysical = 4, // a real machine, not a simulated one
    kDriven = 8,   // a pilot has it now
    kFocus = 16,   // the host wants attention on it
    kFiring = 32,  // emitters hot
};

struct EntityState {
    std::uint32_t id{0};
    std::uint16_t shape{0};
    std::uint8_t team{0};
    std::uint8_t flags{kAlive};
    Vec3 pos;
    Vec3 vel;
    Quat q;
    Rgb8 tint{230, 225, 211};
    float scale{1.0f};
};
inline constexpr std::size_t kEntityBytes = 34;

struct Snapshot {
    double hostTime{0};
    float rate{1}; // host seconds per real second (a simulation may run faster than real time)
    std::uint32_t tick{0};
    std::uint8_t part{0}, parts{1};
    std::vector<EntityState> entities;
};
inline constexpr std::size_t kSnapshotOverhead = 8 + 4 + 4 + 3;
inline constexpr std::size_t kEntitiesPerPacket = (kMaxPayload - kSnapshotOverhead) / kEntityBytes;

enum class EffectKind : std::uint8_t { Laser, Tracer, Link, Ring, Beam, Trail, Spot };

struct Effect {
    EffectKind kind{EffectKind::Beam};
    Rgb8 colour{255, 255, 255};
    Vec3 a, b; // endpoints; a Ring uses a as the centre and b.x as the radius
    float width{1.0f};
    std::uint8_t intensity{255};
};
inline constexpr std::size_t kEffectBytes = 33;

struct Effects {
    double hostTime{0};
    std::uint8_t part{0}, parts{1};
    std::vector<Effect> effects;
};
inline constexpr std::size_t kEffectsPerPacket = (kMaxPayload - 11) / kEffectBytes;

struct InfoItem {
    std::uint32_t id{0};
    std::string name, task;
};
struct Info {
    std::vector<InfoItem> items;
};

enum class Tone : std::uint8_t { Normal, Good, Warn, Alert };
struct StatusItem {
    std::string label, value;
    Tone tone{Tone::Normal};
};
struct Status {
    double hostTime{0};
    std::vector<StatusItem> items;
};

enum class EventKind : std::uint8_t { Info, Brain, Opfor, Outcome, Sky, Order, Pilot };
struct Event {
    double time{0};
    EventKind kind{EventKind::Info};
    std::string text;
};

struct DropIn {
    std::uint32_t entity{0};
};

enum class Drop : std::uint8_t {
    Riding,   // view is in the node; it is still autonomous until a good stopping point
    Control,  // the pilot has it
    Denied,   // it cannot be driven (reason says why)
    Released, // back to autonomy
    Lost,     // the node is gone (destroyed, out of range, link lost)
};
struct DropState {
    std::uint32_t entity{0};
    Drop state{Drop::Riding};
    float eta{0}; // seconds until the expected handover, while riding
    std::string reason;
    // With Control: how the node handles, so the client predicts it exactly (glyder/handling.hpp).
    std::uint8_t model{0};
    float minSpeed{0}, maxSpeed{0}, yawRate{0}, climbRate{0}, accel{0};
};

struct Control {
    std::uint32_t entity{0};
    std::uint32_t seq{0};
    double clientTime{0};
    // Normalised -1..1: forward, right, up, yaw right, pitch up, throttle.
    std::array<float, 6> axes{};
    std::uint16_t buttons{0};
    float lookYaw{0}, lookPitch{0}; // radians: where the pilot's head points relative to the nose
};

struct Order {
    std::string text;
};

struct Ping {
    std::uint32_t id{0};
    double clientTime{0};
};
struct Pong {
    std::uint32_t id{0};
    double clientTime{0};
    double hostTime{0};
};

struct Ack {
    std::uint32_t cumulative{0}; // every reliable seq <= this has arrived
    std::uint32_t mask{0};       // bit i: cumulative + 1 + i has arrived
};

struct TerrainChunk {
    std::uint32_t offset{0}, total{0};
    std::vector<std::uint8_t> bytes;
};
inline constexpr std::size_t kTerrainChunk = 1024;

// A heightfield: w x h samples `cell` metres apart, the corner at (x0, y0), heights in metres.
struct Heightfield {
    std::uint16_t w{0}, h{0};
    float cell{1}, x0{0}, y0{0};
    std::vector<float> z;
    [[nodiscard]] float at(int i, int j) const { return z[static_cast<std::size_t>(j) * w + static_cast<std::size_t>(i)]; }
    [[nodiscard]] float sample(float x, float y) const; // bilinear, clamped
    [[nodiscard]] std::vector<std::uint8_t> encode() const;
    [[nodiscard]] static std::optional<Heightfield> decode(std::span<const std::uint8_t> bytes);
};

struct Empty {};
inline void encode(Writer&, const Empty&) {}

// Encoders write the payload only; decoders read it and return nullopt on malformed input.
void encode(Writer& w, const Beacon& m);
void encode(Writer& w, const Hello& m);
void encode(Writer& w, const Challenge& m);
void encode(Writer& w, const Auth& m);
void encode(Writer& w, const World& m);
void encode(Writer& w, const Snapshot& m);
void encode(Writer& w, const Effects& m);
void encode(Writer& w, const Info& m);
void encode(Writer& w, const Status& m);
void encode(Writer& w, const Event& m);
void encode(Writer& w, const DropIn& m);
void encode(Writer& w, const DropState& m);
void encode(Writer& w, const Control& m);
void encode(Writer& w, const Order& m);
void encode(Writer& w, const Ping& m);
void encode(Writer& w, const Pong& m);
void encode(Writer& w, const Ack& m);
void encode(Writer& w, const TerrainChunk& m);

template <class T> [[nodiscard]] std::optional<T> decode(Reader& r);

// Session keys: from the pairing secret and both nonces. With no secret (a loopback host) the
// tags still detect corruption and stray packets but prove nothing.
[[nodiscard]] std::array<std::uint8_t, 32> pairingKey(std::string_view secret);
[[nodiscard]] std::array<std::uint8_t, 32> authProof(std::span<const std::uint8_t> key, std::span<const std::uint8_t> clientNonce,
                                                     std::span<const std::uint8_t> hostNonce);
[[nodiscard]] std::array<std::uint8_t, 32> sessionKey(std::span<const std::uint8_t> key, std::span<const std::uint8_t> clientNonce,
                                                      std::span<const std::uint8_t> hostNonce);

} // namespace glyder::wire
