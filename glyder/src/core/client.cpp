#include <glyder/client.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <glyder/crypto.hpp>

namespace glyder {

namespace {
constexpr std::array<std::uint8_t, 32> kNoKey{};
constexpr double kBlend = 0.12;      // seconds to blend out a correction
constexpr double kForget = 1.5;      // seconds without news before an entity is dropped
constexpr double kExtrapolate = 0.6; // host seconds of dead reckoning before holding still
} // namespace

Client::Client(std::string name) : name_(std::move(name)) {}

Client::~Client() {
    disconnect();
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

bool Client::open(std::string* error) {
    socket_ = net::Socket::open("0.0.0.0", 0, true, error);
    if (!socket_) return false;
    running_ = true;
    thread_ = std::jthread([this] { loop(); });
    return true;
}

void Client::discover(bool on, std::vector<net::Address> also) {
    std::lock_guard lock(mutex_);
    discovering_ = on;
    also_ = std::move(also);
    lastDiscover_ = 0;
}

std::vector<Found> Client::found() const {
    std::lock_guard lock(mutex_);
    std::vector<Found> out;
    const double t = net::now();
    for (const auto& [a, f] : found_)
        if (t - f.seen < 4.0) out.push_back(f);
    return out;
}

void Client::connect(net::Address host, std::string secret) {
    std::lock_guard lock(mutex_);
    host_ = host;
    secret_ = std::move(secret);
    state_ = State::Connecting;
    error_.clear();
    crypto::randomBytes(nonce_);
    session_ = 0;
    lastHello_ = 0;
    world_.reset();
    tracks_.clear();
    fx_.clear();
    info_.clear();
    events_.clear();
    drops_.clear();
    terrain_.reset();
    terrainBytes_.clear();
    terrainHave_.clear();
    terrainGot_ = 0;
    out_ = {};
    in_ = {};
    clockSet_ = false;
}

void Client::disconnect() {
    std::lock_guard lock(mutex_);
    if (state_ == State::Connected && socket_) socket_->send(host_, net::packet(wire::Type::Bye, session_, 0, 0, wire::Empty{}, key_));
    state_ = State::Idle;
}

Client::State Client::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}
std::string Client::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}
std::optional<wire::World> Client::world() const {
    std::lock_guard lock(mutex_);
    return world_;
}
net::Address Client::host() const {
    std::lock_guard lock(mutex_);
    return host_;
}
double Client::rtt() const {
    std::lock_guard lock(mutex_);
    return rtt_;
}
double Client::sinceHeard(double t) const {
    std::lock_guard lock(mutex_);
    return t - lastHeard_;
}

float Client::terrainProgress() const {
    std::lock_guard lock(mutex_);
    if (!world_ || world_->terrainBytes == 0) return 1.0f;
    return static_cast<float>(terrainGot_) / static_cast<float>(world_->terrainBytes);
}

std::uint64_t Client::worldRevision() const {
    std::lock_guard lock(mutex_);
    return worldRevision_;
}

std::optional<wire::Heightfield> Client::takeTerrain() {
    std::lock_guard lock(mutex_);
    return std::exchange(terrain_, std::nullopt);
}

double Client::hostTimeAt(double t) const {
    std::lock_guard lock(mutex_);
    return clockSet_ ? base_ + (t - baseAt_) * rate_ : 0.0;
}

void Client::sample(double t, std::vector<wire::EntityState>& out) const {
    std::lock_guard lock(mutex_);
    out.clear();
    if (!clockSet_) return;
    const double hostNow = base_ + (t - baseAt_) * rate_;
    out.reserve(tracks_.size());
    for (const auto& [id, tr] : tracks_) {
        wire::EntityState e = tr.last;
        const double ahead = std::clamp(hostNow - tr.hostTime, 0.0, kExtrapolate * std::max(rate_, 1.0));
        e.pos = tr.last.pos + tr.last.vel * static_cast<float>(ahead);
        const float fade = static_cast<float>(std::exp(-(t - tr.errorAt) / kBlend * 3.0));
        e.pos += tr.error * fade;
        const float qt = static_cast<float>(std::clamp((t - tr.shownAt) / kBlend, 0.0, 1.0));
        e.q = slerp(tr.shown, tr.last.q, qt);
        out.push_back(e);
    }
}

std::vector<wire::Effect> Client::effects() const {
    std::lock_guard lock(mutex_);
    return fx_;
}

std::optional<wire::InfoItem> Client::info(std::uint32_t id) const {
    std::lock_guard lock(mutex_);
    const auto it = info_.find(id);
    return it == info_.end() ? std::nullopt : std::optional(it->second);
}

wire::Status Client::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

std::vector<wire::Event> Client::takeEvents() {
    std::lock_guard lock(mutex_);
    std::vector<wire::Event> out(events_.begin(), events_.end());
    events_.clear();
    return out;
}

std::vector<wire::DropState> Client::takeDropStates() {
    std::lock_guard lock(mutex_);
    std::vector<wire::DropState> out(drops_.begin(), drops_.end());
    drops_.clear();
    return out;
}

void Client::sendReliable(wire::Type type, const std::vector<std::uint8_t>& payload) {
    if (state_ != State::Connected) return;
    const std::uint32_t seq = out_.nextSeq();
    wire::Writer w;
    wire::writeHeader(w, {type, static_cast<std::uint8_t>(wire::kReliable | wire::kTagged), session_, seq});
    w.bytes(payload);
    auto bytes = std::move(w.data());
    wire::sign(bytes, key_);
    socket_->send(host_, bytes);
    out_.sent(seq, std::move(bytes), net::now());
}

template <class M> void Client::send(wire::Type type, const M& m) {
    if (state_ != State::Connected) return;
    socket_->send(host_, net::packet(type, session_, 0, 0, m, key_));
}

void Client::dropIn(std::uint32_t entity) {
    std::lock_guard lock(mutex_);
    sendReliable(wire::Type::DropIn, wire::DropIn{entity});
}

void Client::release() {
    std::lock_guard lock(mutex_);
    sendReliable(wire::Type::Release, wire::Empty{});
}

void Client::control(const wire::Control& c) {
    std::lock_guard lock(mutex_);
    wire::Control m = c;
    m.seq = ++controlSeq_;
    send(wire::Type::Control, m);
}

void Client::order(const std::string& text) {
    std::lock_guard lock(mutex_);
    sendReliable(wire::Type::Order, wire::Order{text});
}

void Client::loop() {
    while (running_) {
        if (auto d = socket_->receive(2)) {
            handle(*d, net::now());
            for (int i = 0; i < 256; ++i) {
                auto more = socket_->receive(0);
                if (!more) break;
                handle(*more, net::now());
            }
        }
        const double t = net::now();
        std::lock_guard lock(mutex_);
        if (discovering_ && t - lastDiscover_ > 1.0) {
            lastDiscover_ = t;
            const auto p = net::packet(wire::Type::Discover, 0, 0, 0, wire::Empty{}, kNoKey);
            socket_->send(net::Address::broadcast(wire::kDefaultPort), p);
            socket_->send({0x7F000001u, wire::kDefaultPort}, p);
            for (const auto& a : also_) socket_->send(a, p);
        }
        if (state_ == State::Connecting && t - lastHello_ > 0.5) {
            lastHello_ = t;
            socket_->send(host_, net::packet(wire::Type::Hello, 0, 0, 0, wire::Hello{name_, nonce_}, kNoKey));
        }
        if (state_ == State::Connected) {
            for (auto& p : out_.due(t)) socket_->send(host_, p);
            if (t - lastPing_ > 0.5) {
                lastPing_ = t;
                send(wire::Type::Ping, wire::Ping{++pingId_, t});
            }
            if (t - lastHeard_ > 6.0) {
                state_ = State::Failed;
                error_ = "lost the host";
            }
        }
        for (auto it = tracks_.begin(); it != tracks_.end();) it = t - it->second.arrived > kForget ? tracks_.erase(it) : std::next(it);
    }
}

void Client::handle(const net::Datagram& d, double t) {
    wire::Reader r(d.bytes);
    const auto h = wire::readHeader(r);
    if (!h) return;
    ++packetsIn_;
    std::lock_guard lock(mutex_);
    const auto payload = std::span(d.bytes).subspan(wire::kHeaderBytes, d.bytes.size() - wire::kHeaderBytes - wire::kTagBytes);
    wire::Reader body(payload);

    switch (h->type) {
    case wire::Type::Beacon:
        if (wire::verify(d.bytes, kNoKey))
            if (auto b = wire::decode<wire::Beacon>(body)) {
                net::Address a{d.from.ip, b->port};
                found_[a] = {a, *b, t};
            }
        return;
    case wire::Type::Challenge:
        if (state_ == State::Connecting && d.from == host_ && wire::verify(d.bytes, kNoKey))
            if (auto c = wire::decode<wire::Challenge>(body)) {
                const auto pk = wire::pairingKey(secret_);
                key_ = wire::sessionKey(pk, nonce_, c->nonce);
                socket_->send(host_, net::packet(wire::Type::Auth, 0, 0, 0, wire::Auth{wire::authProof(pk, nonce_, c->nonce)}, kNoKey));
            }
        return;
    case wire::Type::Denied:
        if (d.from == host_ && wire::verify(d.bytes, kNoKey)) {
            const auto m = wire::decode<wire::Order>(body);
            state_ = State::Failed;
            error_ = m ? m->text : "denied";
        }
        return;
    default: break;
    }

    if (d.from != host_ || state_ == State::Idle || !wire::verify(d.bytes, key_)) return;
    lastHeard_ = t;

    if (h->type == wire::Type::Welcome) {
        if (auto w = wire::decode<wire::World>(body)) {
            if (state_ == State::Connected && world_ && w->session == session_) {
                // Already in: a scene change, or a repeat of the same Welcome (ignored).
                const auto& o = *world_;
                if (o.title == w->title && o.subtitle == w->subtitle && o.environment == w->environment && o.radius == w->radius &&
                    o.groundZ == w->groundZ && o.terrainBytes == w->terrainBytes && o.help == w->help && o.originLat == w->originLat &&
                    o.originLon == w->originLon)
                    return;
                ++worldRevision_;
                tracks_.clear();
                fx_.clear();
                info_.clear();
                terrain_.reset();
            }
            world_ = *w;
            session_ = w->session;
            state_ = State::Connected;
            terrainBytes_.assign(w->terrainBytes, 0);
            terrainHave_.assign((w->terrainBytes + wire::kTerrainChunk - 1) / wire::kTerrainChunk, false);
            terrainGot_ = 0;
            if (!clockSet_) {
                base_ = w->hostTime;
                baseAt_ = t;
                clockSet_ = true;
            }
        }
        return;
    }
    if (h->session != session_) return;

    if (h->flags & wire::kReliable) {
        auto ready = in_.accept(h->seq, h->type, payload);
        socket_->send(host_, net::packet(wire::Type::Ack, session_, 0, 0, in_.ack(), key_));
        for (auto& [type, bytes] : ready) {
            wire::Reader rr(bytes);
            if (type == wire::Type::Event) {
                if (auto e = wire::decode<wire::Event>(rr)) {
                    events_.push_back(std::move(*e));
                    while (events_.size() > 256) events_.pop_front();
                }
            } else if (type == wire::Type::DropState) {
                if (auto s = wire::decode<wire::DropState>(rr)) drops_.push_back(std::move(*s));
            } else if (type == wire::Type::Terrain) {
                if (auto c = wire::decode<wire::TerrainChunk>(rr)) {
                    if (c->total != terrainBytes_.size()) continue;
                    const std::size_t idx = c->offset / wire::kTerrainChunk;
                    if (idx < terrainHave_.size() && !terrainHave_[idx]) {
                        std::copy(c->bytes.begin(), c->bytes.end(), terrainBytes_.begin() + static_cast<std::ptrdiff_t>(c->offset));
                        terrainHave_[idx] = true;
                        terrainGot_ += c->bytes.size();
                        if (terrainGot_ == terrainBytes_.size()) terrain_ = wire::Heightfield::decode(terrainBytes_);
                    }
                }
            }
        }
        return;
    }

    switch (h->type) {
    case wire::Type::Snapshot: {
        auto s = wire::decode<wire::Snapshot>(body);
        if (!s) return;
        // Keep the host clock: advance at the host's rate, nudge toward each new frame.
        rate_ = std::clamp(static_cast<double>(s->rate), 0.0, 64.0);
        const double est = base_ + (t - baseAt_) * rate_;
        const double err = s->hostTime - est;
        if (!clockSet_ || std::fabs(err) > std::max(0.5, rate_ * 0.5)) {
            base_ = s->hostTime;
            baseAt_ = t;
            clockSet_ = true;
        } else if (err > 0) {
            // Never run the clock backwards; frames that arrive early pull it forward a little.
            base_ = est + err * 0.25;
            baseAt_ = t;
        }
        for (const auto& e : s->entities) {
            auto [it, isNew] = tracks_.try_emplace(e.id);
            Track& tr = it->second;
            if (!isNew && s->hostTime < tr.hostTime) continue; // stale
            if (!isNew && s->hostTime == tr.hostTime) {
                tr.arrived = t;
                continue;
            }
            if (!isNew) {
                // Where the entity is being shown now, so the correction starts from there.
                const double ahead = std::clamp(base_ + (t - baseAt_) * rate_ - tr.hostTime, 0.0, kExtrapolate * std::max(rate_, 1.0));
                const float fade = static_cast<float>(std::exp(-(t - tr.errorAt) / kBlend * 3.0));
                const Vec3 shownPos = tr.last.pos + tr.last.vel * static_cast<float>(ahead) + tr.error * fade;
                const double aheadNew = std::clamp(base_ + (t - baseAt_) * rate_ - s->hostTime, 0.0, kExtrapolate * std::max(rate_, 1.0));
                const Vec3 newPos = e.pos + e.vel * static_cast<float>(aheadNew);
                tr.error = shownPos - newPos;
                if (length(tr.error) > 50.0f + length(e.vel) * 0.5f) tr.error = {}; // a jump (teleport, respawn): don't smear it
                tr.errorAt = t;
                const float qt = static_cast<float>(std::clamp((t - tr.shownAt) / kBlend, 0.0, 1.0));
                tr.shown = slerp(tr.shown, tr.last.q, qt);
                tr.shownAt = t;
            } else {
                tr.shown = e.q;
                tr.shownAt = t;
                tr.errorAt = t;
            }
            tr.last = e;
            tr.hostTime = s->hostTime;
            tr.arrived = t;
        }
        return;
    }
    case wire::Type::Effects: {
        auto f = wire::decode<wire::Effects>(body);
        if (!f) return;
        if (f->hostTime > fxTime_) {
            fxTime_ = f->hostTime;
            fx_.clear();
        }
        if (f->hostTime == fxTime_) fx_.insert(fx_.end(), f->effects.begin(), f->effects.end());
        return;
    }
    case wire::Type::Info:
        if (auto in = wire::decode<wire::Info>(body))
            for (auto& it : in->items) info_[it.id] = std::move(it);
        return;
    case wire::Type::Status:
        if (auto st = wire::decode<wire::Status>(body)) status_ = std::move(*st);
        return;
    case wire::Type::Ack:
        if (auto a = wire::decode<wire::Ack>(body)) out_.acked(*a, t);
        return;
    case wire::Type::Pong:
        if (auto p = wire::decode<wire::Pong>(body)) {
            const double sample = t - p->clientTime;
            rtt_ = rtt_ == 0.0 ? sample : rtt_ * 0.8 + sample * 0.2;
        }
        return;
    case wire::Type::Bye:
        state_ = State::Failed;
        error_ = "the host closed the session";
        return;
    default: return;
    }
}

} // namespace glyder
