#include <glyder/wire.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include <glyder/crypto.hpp>

namespace glyder::wire {

// ---------------------------------------------------------------------------------------------
// Byte codec

void Writer::u16(std::uint16_t v) {
    b_.push_back(static_cast<std::uint8_t>(v));
    b_.push_back(static_cast<std::uint8_t>(v >> 8));
}
void Writer::u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void Writer::u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void Writer::f32(float v) {
    u32(std::bit_cast<std::uint32_t>(v));
}
void Writer::f64(double v) {
    u64(std::bit_cast<std::uint64_t>(v));
}
void Writer::str8(std::string_view s) {
    const std::size_t n = std::min<std::size_t>(s.size(), 255);
    u8(static_cast<std::uint8_t>(n));
    b_.insert(b_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}
void Writer::str16(std::string_view s) {
    const std::size_t n = std::min<std::size_t>(s.size(), 65535);
    u16(static_cast<std::uint16_t>(n));
    b_.insert(b_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

bool Reader::need(std::size_t n) {
    if (!ok_ || s_.size() - p_ < n) {
        ok_ = false;
        return false;
    }
    return true;
}
std::uint8_t Reader::u8() {
    return need(1) ? s_[p_++] : 0;
}
std::uint16_t Reader::u16() {
    if (!need(2)) return 0;
    const auto v = static_cast<std::uint16_t>(s_[p_] | (s_[p_ + 1] << 8));
    p_ += 2;
    return v;
}
std::uint32_t Reader::u32() {
    if (!need(4)) return 0;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::uint32_t{s_[p_ + static_cast<std::size_t>(i)]} << (8 * i);
    p_ += 4;
    return v;
}
std::uint64_t Reader::u64() {
    if (!need(8)) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= std::uint64_t{s_[p_ + static_cast<std::size_t>(i)]} << (8 * i);
    p_ += 8;
    return v;
}
float Reader::f32() {
    return std::bit_cast<float>(u32());
}
double Reader::f64() {
    return std::bit_cast<double>(u64());
}
std::string Reader::str8() {
    const std::size_t n = u8();
    if (!need(n)) return {};
    std::string s(reinterpret_cast<const char*>(s_.data() + p_), n);
    p_ += n;
    return s;
}
std::string Reader::str16() {
    const std::size_t n = u16();
    if (!need(n)) return {};
    std::string s(reinterpret_cast<const char*>(s_.data() + p_), n);
    p_ += n;
    return s;
}
std::span<const std::uint8_t> Reader::bytes(std::size_t n) {
    if (!need(n)) return {};
    auto s = s_.subspan(p_, n);
    p_ += n;
    return s;
}

// ---------------------------------------------------------------------------------------------
// Compact numbers

namespace {

std::uint16_t toHalf(float f) {
    const std::uint32_t x = std::bit_cast<std::uint32_t>(f);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = static_cast<int>((x >> 23) & 0xFFu) - 127 + 15;
    std::uint32_t mant = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFFu) == 0xFFu) return static_cast<std::uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
    if (exp >= 31) return static_cast<std::uint16_t>(sign | 0x7BFFu); // clamp to the largest finite
    if (exp <= 0) {
        if (exp < -10) return static_cast<std::uint16_t>(sign);
        mant |= 0x800000u;
        const int shift = 14 - exp;
        std::uint32_t half = mant >> shift;
        if ((mant >> (shift - 1)) & 1u) ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = sign | (static_cast<std::uint32_t>(exp) << 10) | (mant >> 13);
    if (mant & 0x1000u) ++half; // round half up
    return static_cast<std::uint16_t>(half);
}

float fromHalf(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1Fu;
    std::uint32_t mant = h & 0x3FFu;
    std::uint32_t x;
    if (exp == 0) {
        if (mant == 0) {
            x = sign;
        } else {
            int e = -1;
            do {
                ++e;
                mant <<= 1;
            } while (!(mant & 0x400u));
            x = sign | (static_cast<std::uint32_t>(127 - 15 - e) << 23) | ((mant & 0x3FFu) << 13);
        }
    } else if (exp == 31) {
        x = sign | 0x7F800000u | (mant << 13);
    } else {
        x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    return std::bit_cast<float>(x);
}

std::uint8_t packScale(float s) {
    const float code = 128.0f + 16.0f * std::log2(std::max(s, 1e-6f));
    return static_cast<std::uint8_t>(std::clamp(std::lround(code), 0L, 255L));
}
float unpackScale(std::uint8_t c) {
    return std::exp2((static_cast<float>(c) - 128.0f) / 16.0f);
}

} // namespace

std::uint32_t packQuat(Quat q) {
    q = normalize(q);
    const float c[4] = {q.x, q.y, q.z, q.w};
    int big = 0;
    for (int i = 1; i < 4; ++i)
        if (std::fabs(c[i]) > std::fabs(c[big])) big = i;
    const float sign = c[big] < 0 ? -1.0f : 1.0f;
    std::uint32_t v = static_cast<std::uint32_t>(big) << 30;
    int shift = 20;
    for (int i = 0; i < 4; ++i) {
        if (i == big) continue;
        const float n = clamp(c[i] * sign / 0.70710678f, -1.0f, 1.0f);
        const auto q10 = static_cast<std::uint32_t>(std::lround((n + 1.0f) * 0.5f * 1023.0f));
        v |= q10 << shift;
        shift -= 10;
    }
    return v;
}

Quat unpackQuat(std::uint32_t v) {
    const int big = static_cast<int>(v >> 30);
    float c[4]{};
    float sum = 0;
    // The three smaller components, in order, at bits 20, 10 and 0.
    constexpr unsigned kShifts[3] = {20u, 10u, 0u};
    for (int i = 0, k = 0; i < 4 && k < 3; ++i) {
        if (i == big) continue;
        const float n = static_cast<float>((v >> kShifts[k++]) & 0x3FFu) / 1023.0f * 2.0f - 1.0f;
        c[i] = n * 0.70710678f;
        sum += c[i] * c[i];
    }
    c[big] = std::sqrt(std::max(0.0f, 1.0f - sum));
    return normalize(Quat{c[0], c[1], c[2], c[3]});
}

// ---------------------------------------------------------------------------------------------
// Packets

void writeHeader(Writer& w, const Header& h) {
    w.u32(kMagic);
    w.u8(kVersion);
    w.u8(static_cast<std::uint8_t>(h.type));
    w.u8(h.flags);
    w.u8(0);
    w.u32(h.session);
    w.u32(h.seq);
}

std::optional<Header> readHeader(Reader& r) {
    if (r.u32() != kMagic) return std::nullopt;
    if (r.u8() != kVersion) return std::nullopt;
    Header h;
    const std::uint8_t type = r.u8();
    if (type < static_cast<std::uint8_t>(Type::Discover) || type > static_cast<std::uint8_t>(Type::Denied)) return std::nullopt;
    h.type = static_cast<Type>(type);
    h.flags = r.u8();
    r.u8();
    h.session = r.u32();
    h.seq = r.u32();
    if (!r.ok()) return std::nullopt;
    return h;
}

void sign(std::vector<std::uint8_t>& packet, std::span<const std::uint8_t> key) {
    const auto tag = crypto::hmac(key, packet);
    packet.insert(packet.end(), tag.begin(), tag.begin() + kTagBytes);
}

bool verify(std::span<const std::uint8_t> packet, std::span<const std::uint8_t> key) {
    if (packet.size() < kHeaderBytes + kTagBytes) return false;
    const auto body = packet.first(packet.size() - kTagBytes);
    const auto tag = crypto::hmac(key, body);
    return crypto::equal(std::span(tag).first(kTagBytes), packet.last(kTagBytes));
}

// ---------------------------------------------------------------------------------------------
// Messages

void encode(Writer& w, const Beacon& m) {
    w.str8(m.name);
    w.str8(m.title);
    w.u16(m.port);
    w.u16(m.entities);
    w.u8(m.pairing ? 1 : 0);
    w.u8(m.environment);
}
template <> std::optional<Beacon> decode(Reader& r) {
    Beacon m;
    m.name = r.str8();
    m.title = r.str8();
    m.port = r.u16();
    m.entities = r.u16();
    m.pairing = r.u8() != 0;
    m.environment = r.u8();
    return r.ok() ? std::optional(m) : std::nullopt;
}

void encode(Writer& w, const Hello& m) {
    w.str8(m.client);
    w.bytes(m.nonce);
}
template <> std::optional<Hello> decode(Reader& r) {
    Hello m;
    m.client = r.str8();
    const auto n = r.bytes(16);
    if (!r.ok()) return std::nullopt;
    std::copy(n.begin(), n.end(), m.nonce.begin());
    return m;
}

void encode(Writer& w, const Challenge& m) {
    w.bytes(m.nonce);
    w.u8(m.pairing ? 1 : 0);
}
template <> std::optional<Challenge> decode(Reader& r) {
    Challenge m;
    const auto n = r.bytes(16);
    m.pairing = r.u8() != 0;
    if (!r.ok()) return std::nullopt;
    std::copy(n.begin(), n.end(), m.nonce.begin());
    return m;
}

void encode(Writer& w, const Auth& m) {
    w.bytes(m.proof);
}
template <> std::optional<Auth> decode(Reader& r) {
    Auth m;
    const auto p = r.bytes(32);
    if (!r.ok()) return std::nullopt;
    std::copy(p.begin(), p.end(), m.proof.begin());
    return m;
}

void encode(Writer& w, const World& m) {
    w.u32(m.session);
    w.f64(m.hostTime);
    w.u16(m.snapshotHz);
    w.u8(m.environment);
    w.str8(m.title);
    w.str8(m.subtitle);
    w.f64(m.originLat);
    w.f64(m.originLon);
    w.f32(m.groundZ);
    w.f32(m.radius);
    w.u32(m.terrainBytes);
    w.str16(m.help);
}
template <> std::optional<World> decode(Reader& r) {
    World m;
    m.session = r.u32();
    m.hostTime = r.f64();
    m.snapshotHz = r.u16();
    m.environment = r.u8();
    m.title = r.str8();
    m.subtitle = r.str8();
    m.originLat = r.f64();
    m.originLon = r.f64();
    m.groundZ = r.f32();
    m.radius = r.f32();
    m.terrainBytes = r.u32();
    m.help = r.str16();
    return r.ok() ? std::optional(m) : std::nullopt;
}

namespace {
void writeEntity(Writer& w, const EntityState& e) {
    w.u32(e.id);
    w.u16(e.shape);
    w.u8(e.team);
    w.u8(e.flags);
    w.f32(e.pos.x);
    w.f32(e.pos.y);
    w.f32(e.pos.z);
    w.u16(toHalf(e.vel.x));
    w.u16(toHalf(e.vel.y));
    w.u16(toHalf(e.vel.z));
    w.u32(packQuat(e.q));
    w.u8(e.tint.r);
    w.u8(e.tint.g);
    w.u8(e.tint.b);
    w.u8(packScale(e.scale));
}
EntityState readEntity(Reader& r) {
    EntityState e;
    e.id = r.u32();
    e.shape = r.u16();
    e.team = r.u8();
    e.flags = r.u8();
    e.pos = {r.f32(), r.f32(), r.f32()};
    const float vx = fromHalf(r.u16()), vy = fromHalf(r.u16()), vz = fromHalf(r.u16());
    e.vel = {vx, vy, vz};
    e.q = unpackQuat(r.u32());
    e.tint = {r.u8(), r.u8(), r.u8()};
    e.scale = unpackScale(r.u8());
    return e;
}
} // namespace

void encode(Writer& w, const Snapshot& m) {
    w.f64(m.hostTime);
    w.f32(m.rate);
    w.u32(m.tick);
    w.u8(m.part);
    w.u8(m.parts);
    w.u8(static_cast<std::uint8_t>(std::min<std::size_t>(m.entities.size(), 255)));
    for (std::size_t i = 0; i < m.entities.size() && i < 255; ++i) writeEntity(w, m.entities[i]);
}
template <> std::optional<Snapshot> decode(Reader& r) {
    Snapshot m;
    m.hostTime = r.f64();
    m.rate = r.f32();
    m.tick = r.u32();
    m.part = r.u8();
    m.parts = r.u8();
    const std::size_t n = r.u8();
    if (!r.ok() || r.remaining() < n * kEntityBytes) return std::nullopt;
    m.entities.reserve(n);
    for (std::size_t i = 0; i < n; ++i) m.entities.push_back(readEntity(r));
    return r.ok() ? std::optional(std::move(m)) : std::nullopt;
}

void encode(Writer& w, const Effects& m) {
    w.f64(m.hostTime);
    w.u8(m.part);
    w.u8(m.parts);
    w.u8(static_cast<std::uint8_t>(std::min<std::size_t>(m.effects.size(), 255)));
    for (std::size_t i = 0; i < m.effects.size() && i < 255; ++i) {
        const Effect& e = m.effects[i];
        w.u8(static_cast<std::uint8_t>(e.kind));
        w.u8(e.colour.r);
        w.u8(e.colour.g);
        w.u8(e.colour.b);
        for (float v : {e.a.x, e.a.y, e.a.z, e.b.x, e.b.y, e.b.z, e.width}) w.f32(v);
        w.u8(e.intensity);
    }
}
template <> std::optional<Effects> decode(Reader& r) {
    Effects m;
    m.hostTime = r.f64();
    m.part = r.u8();
    m.parts = r.u8();
    const std::size_t n = r.u8();
    if (!r.ok() || r.remaining() < n * kEffectBytes) return std::nullopt;
    for (std::size_t i = 0; i < n; ++i) {
        Effect e;
        e.kind = static_cast<EffectKind>(r.u8());
        e.colour = {r.u8(), r.u8(), r.u8()};
        e.a = {r.f32(), r.f32(), r.f32()};
        e.b = {r.f32(), r.f32(), r.f32()};
        e.width = r.f32();
        e.intensity = r.u8();
        m.effects.push_back(e);
    }
    return r.ok() ? std::optional(std::move(m)) : std::nullopt;
}

void encode(Writer& w, const Info& m) {
    w.u8(static_cast<std::uint8_t>(std::min<std::size_t>(m.items.size(), 255)));
    for (std::size_t i = 0; i < m.items.size() && i < 255; ++i) {
        w.u32(m.items[i].id);
        w.str8(m.items[i].name);
        w.str8(m.items[i].task);
    }
}
template <> std::optional<Info> decode(Reader& r) {
    Info m;
    const std::size_t n = r.u8();
    for (std::size_t i = 0; i < n && r.ok(); ++i) {
        InfoItem it;
        it.id = r.u32();
        it.name = r.str8();
        it.task = r.str8();
        m.items.push_back(std::move(it));
    }
    return r.ok() ? std::optional(std::move(m)) : std::nullopt;
}

void encode(Writer& w, const Status& m) {
    w.f64(m.hostTime);
    w.u8(static_cast<std::uint8_t>(std::min<std::size_t>(m.items.size(), 255)));
    for (std::size_t i = 0; i < m.items.size() && i < 255; ++i) {
        w.str8(m.items[i].label);
        w.str8(m.items[i].value);
        w.u8(static_cast<std::uint8_t>(m.items[i].tone));
    }
}
template <> std::optional<Status> decode(Reader& r) {
    Status m;
    m.hostTime = r.f64();
    const std::size_t n = r.u8();
    for (std::size_t i = 0; i < n && r.ok(); ++i) {
        StatusItem it;
        it.label = r.str8();
        it.value = r.str8();
        it.tone = static_cast<Tone>(r.u8());
        m.items.push_back(std::move(it));
    }
    return r.ok() ? std::optional(std::move(m)) : std::nullopt;
}

void encode(Writer& w, const Event& m) {
    w.f64(m.time);
    w.u8(static_cast<std::uint8_t>(m.kind));
    w.str16(m.text);
}
template <> std::optional<Event> decode(Reader& r) {
    Event m;
    m.time = r.f64();
    m.kind = static_cast<EventKind>(r.u8());
    m.text = r.str16();
    return r.ok() ? std::optional(std::move(m)) : std::nullopt;
}

void encode(Writer& w, const DropIn& m) {
    w.u32(m.entity);
}
template <> std::optional<DropIn> decode(Reader& r) {
    DropIn m{r.u32()};
    return r.ok() ? std::optional(m) : std::nullopt;
}

void encode(Writer& w, const DropState& m) {
    w.u32(m.entity);
    w.u8(static_cast<std::uint8_t>(m.state));
    w.f32(m.eta);
    w.str8(m.reason);
    w.u8(m.model);
    for (float v : {m.minSpeed, m.maxSpeed, m.yawRate, m.climbRate, m.accel}) w.f32(v);
}
template <> std::optional<DropState> decode(Reader& r) {
    DropState m;
    m.entity = r.u32();
    m.state = static_cast<Drop>(r.u8());
    m.eta = r.f32();
    m.reason = r.str8();
    m.model = r.u8();
    m.minSpeed = r.f32();
    m.maxSpeed = r.f32();
    m.yawRate = r.f32();
    m.climbRate = r.f32();
    m.accel = r.f32();
    return r.ok() && static_cast<int>(m.state) <= static_cast<int>(Drop::Lost) ? std::optional(std::move(m)) : std::nullopt;
}

void encode(Writer& w, const Control& m) {
    w.u32(m.entity);
    w.u32(m.seq);
    w.f64(m.clientTime);
    for (float a : m.axes) w.i16(static_cast<std::int16_t>(std::lround(clamp(a, -1.0f, 1.0f) * 32767.0f)));
    w.u16(m.buttons);
    w.u16(toHalf(m.lookYaw));
    w.u16(toHalf(m.lookPitch));
}
template <> std::optional<Control> decode(Reader& r) {
    Control m;
    m.entity = r.u32();
    m.seq = r.u32();
    m.clientTime = r.f64();
    for (float& a : m.axes) a = static_cast<float>(r.i16()) / 32767.0f;
    m.buttons = r.u16();
    m.lookYaw = fromHalf(r.u16());
    m.lookPitch = fromHalf(r.u16());
    return r.ok() ? std::optional(m) : std::nullopt;
}

void encode(Writer& w, const Order& m) {
    w.str16(m.text);
}
template <> std::optional<Order> decode(Reader& r) {
    Order m{r.str16()};
    return r.ok() ? std::optional(std::move(m)) : std::nullopt;
}

void encode(Writer& w, const Ping& m) {
    w.u32(m.id);
    w.f64(m.clientTime);
}
template <> std::optional<Ping> decode(Reader& r) {
    Ping m;
    m.id = r.u32();
    m.clientTime = r.f64();
    return r.ok() ? std::optional(m) : std::nullopt;
}

void encode(Writer& w, const Pong& m) {
    w.u32(m.id);
    w.f64(m.clientTime);
    w.f64(m.hostTime);
}
template <> std::optional<Pong> decode(Reader& r) {
    Pong m;
    m.id = r.u32();
    m.clientTime = r.f64();
    m.hostTime = r.f64();
    return r.ok() ? std::optional(m) : std::nullopt;
}

void encode(Writer& w, const Ack& m) {
    w.u32(m.cumulative);
    w.u32(m.mask);
}
template <> std::optional<Ack> decode(Reader& r) {
    Ack m;
    m.cumulative = r.u32();
    m.mask = r.u32();
    return r.ok() ? std::optional(m) : std::nullopt;
}

void encode(Writer& w, const TerrainChunk& m) {
    w.u32(m.offset);
    w.u32(m.total);
    w.u16(static_cast<std::uint16_t>(m.bytes.size()));
    w.bytes(m.bytes);
}
template <> std::optional<TerrainChunk> decode(Reader& r) {
    TerrainChunk m;
    m.offset = r.u32();
    m.total = r.u32();
    const std::size_t n = r.u16();
    const auto b = r.bytes(n);
    if (!r.ok() || m.offset > m.total || n > m.total - m.offset) return std::nullopt;
    m.bytes.assign(b.begin(), b.end());
    return m;
}

// ---------------------------------------------------------------------------------------------
// Heightfield

float Heightfield::sample(float x, float y) const {
    if (w < 2 || h < 2) return z.empty() ? 0.0f : z.front();
    const float fx = clamp((x - x0) / cell, 0.0f, static_cast<float>(w - 1) - 1e-3f);
    const float fy = clamp((y - y0) / cell, 0.0f, static_cast<float>(h - 1) - 1e-3f);
    const int i = static_cast<int>(fx), j = static_cast<int>(fy);
    const float tx = fx - static_cast<float>(i), ty = fy - static_cast<float>(j);
    return lerp(lerp(at(i, j), at(i + 1, j), tx), lerp(at(i, j + 1), at(i + 1, j + 1), tx), ty);
}

std::vector<std::uint8_t> Heightfield::encode() const {
    Writer out;
    out.u16(w);
    out.u16(h);
    out.f32(cell);
    out.f32(x0);
    out.f32(y0);
    float lo = 0, hi = 0;
    if (!z.empty()) {
        const auto [a, b] = std::minmax_element(z.begin(), z.end());
        lo = *a;
        hi = *b;
    }
    const float scale = hi > lo ? (hi - lo) / 65535.0f : 1.0f;
    out.f32(lo);
    out.f32(scale);
    for (float v : z) out.u16(static_cast<std::uint16_t>(std::lround((v - lo) / scale)));
    return out.data();
}

std::optional<Heightfield> Heightfield::decode(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    Heightfield f;
    f.w = r.u16();
    f.h = r.u16();
    f.cell = r.f32();
    f.x0 = r.f32();
    f.y0 = r.f32();
    const float lo = r.f32(), scale = r.f32();
    const std::size_t n = static_cast<std::size_t>(f.w) * f.h;
    if (!r.ok() || r.remaining() != n * 2 || f.cell <= 0) return std::nullopt;
    f.z.resize(n);
    for (float& v : f.z) v = lo + static_cast<float>(r.u16()) * scale;
    return f;
}

// ---------------------------------------------------------------------------------------------
// Keys

std::array<std::uint8_t, 32> pairingKey(std::string_view secret) {
    crypto::Sha256 s;
    s.update("glyder pairing v1\n");
    s.update(secret);
    return s.finish();
}

namespace {
std::array<std::uint8_t, 32> labelled(std::span<const std::uint8_t> key, std::string_view label, std::span<const std::uint8_t> a,
                                      std::span<const std::uint8_t> b) {
    std::vector<std::uint8_t> msg(label.begin(), label.end());
    msg.insert(msg.end(), a.begin(), a.end());
    msg.insert(msg.end(), b.begin(), b.end());
    return crypto::hmac(key, msg);
}
} // namespace

std::array<std::uint8_t, 32> authProof(std::span<const std::uint8_t> key, std::span<const std::uint8_t> clientNonce,
                                       std::span<const std::uint8_t> hostNonce) {
    return labelled(key, "glyder auth", clientNonce, hostNonce);
}

std::array<std::uint8_t, 32> sessionKey(std::span<const std::uint8_t> key, std::span<const std::uint8_t> clientNonce,
                                        std::span<const std::uint8_t> hostNonce) {
    return labelled(key, "glyder session", clientNonce, hostNonce);
}

} // namespace glyder::wire
