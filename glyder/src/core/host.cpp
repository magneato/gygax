#include <glyder/host.hpp>

#include <algorithm>
#include <cstdio>
#include <utility>

#include <glyder/crypto.hpp>

namespace glyder {

namespace {
constexpr std::array<std::uint8_t, 32> kNoKey{};
constexpr double kSessionTimeout = 6.0;
constexpr int kMaxFailures = 5;
constexpr double kFailureWindow = 60.0;
constexpr std::size_t kReliableWindow = 48;

template <class M> std::vector<std::uint8_t> plain(wire::Type type, const M& m) {
    return net::packet(type, 0, 0, 0, m, kNoKey);
}
} // namespace

struct Host::Session {
    net::Address addr;
    std::uint32_t id{0};
    std::string client;
    std::array<std::uint8_t, 16> clientNonce{}, hostNonce{};
    std::array<std::uint8_t, 32> key{};
    bool live{false};
    double lastHeard{0};
    net::ReliableOut out;
    net::ReliableIn in;
    std::deque<std::pair<wire::Type, std::vector<std::uint8_t>>> queued; // reliable payloads waiting for window
    // Drop-in.
    std::uint32_t want{0}; // requested entity (riding or controlling)
    bool controlling{false};
    bool releaseRequested{false};
    std::string lastReason;
    double lastDropSent{0};
    wire::Control control;
    double controlAt{-1e9};
    bool gone{false};
};

Host::Host(HostOptions options) : options_(std::move(options)) {}

Host::~Host() {
    stop();
}

bool Host::start(std::string* error) {
    socket_ = net::Socket::open(options_.bind, options_.port, true, error);
    if (!socket_) return false;
    port_ = socket_->port();
    const auto bindAddr = net::Address::parse(options_.bind, port_);
    const bool loopback = bindAddr && bindAddr->loopback();
    if (!options_.secret.empty()) {
        code_ = options_.secret;
    } else if (!options_.open && !loopback) {
        std::array<std::uint8_t, 4> r{};
        crypto::randomBytes(r);
        const std::uint32_t v =
            (std::uint32_t{r[0]} | std::uint32_t{r[1]} << 8 | std::uint32_t{r[2]} << 16 | std::uint32_t{r[3]} << 24) % 1000000u;
        char buf[8];
        std::snprintf(buf, sizeof buf, "%06u", v);
        code_ = buf;
    }
    pairKey_ = wire::pairingKey(code_);
    running_ = true;
    thread_ = std::jthread([this] { loop(); });
    return true;
}

void Host::stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto& [addr, s] : sessions_)
        if (s->live) socket_->send(addr, net::packet(wire::Type::Bye, s->id, 0, 0, wire::Empty{}, s->key));
    sessions_.clear();
    socket_.reset();
}

std::size_t Host::clients() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(sessions_.begin(), sessions_.end(), [](const auto& kv) { return kv.second->live; }));
}

// ---------------------------------------------------------------------------------------------
// Simulation thread

void Host::setTerrain(const wire::Heightfield& terrain) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    terrain_ = terrain.encode();
}

void Host::publish(double simTime, std::vector<wire::EntityState> entities, std::vector<wire::Effect> effects) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const double wall = net::now();
    if (haveFrame_ && wall > frameWall_ + 1e-4 && simTime >= frameTime_) {
        const double r = (simTime - frameTime_) / (wall - frameWall_);
        rate_ = std::clamp(rate_ * 0.8 + r * 0.2, 0.0, 64.0);
    }
    frameTime_ = simTime;
    frameWall_ = wall;
    haveFrame_ = true;
    ++tick_;
    for (auto& e : entities)
        for (const auto& [addr, s] : sessions_)
            if (s->controlling && s->want == e.id) e.flags |= wire::kDriven;
    entities_ = std::move(entities);
    effects_ = std::move(effects);
}

void Host::info(std::uint32_t id, std::string name, std::string task) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto& i = info_[id];
    i.id = id;
    i.name = std::move(name);
    i.task = std::move(task);
}

void Host::status(std::vector<wire::StatusItem> items) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    status_ = std::move(items);
}

void Host::event(wire::EventKind kind, std::string text, double time) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    wire::Event e{time, kind, std::move(text)};
    const double t = net::now();
    for (auto& [addr, s] : sessions_)
        if (s->live) sendReliable(*s, wire::Type::Event, e, t);
}

std::vector<std::string> Host::takeOrders() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return std::exchange(orders_, {});
}

bool Host::driven(std::uint32_t id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto& [addr, s] : sessions_)
        if (s->controlling && s->want == id) return true;
    return false;
}

void Host::drop(Session& s, const wire::DropState& d) {
    sendReliable(s, wire::Type::DropState, d, net::now());
    s.lastReason = d.reason;
    s.lastDropSent = net::now();
}

void Host::service(Pilot& pilot, double dt) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const double t = net::now();
    for (std::uint32_t id : std::exchange(pendingGive_, {}))
        if (pilot.exists(id)) pilot.give(id);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        Session& s = *it->second;
        const std::uint32_t id = s.want;
        if (id != 0) {
            if (s.gone || s.releaseRequested) {
                if (s.controlling) pilot.give(id);
                if (!s.gone) drop(s, {id, wire::Drop::Released, 0, "autonomy resumed"});
                s.want = 0;
                s.controlling = false;
                s.releaseRequested = false;
            } else if (!pilot.exists(id)) {
                if (s.controlling) pilot.give(id);
                drop(s, {id, wire::Drop::Lost, 0, "the node is gone"});
                s.want = 0;
                s.controlling = false;
            } else if (!s.controlling) {
                bool taken = false;
                for (const auto& [a2, other] : sessions_)
                    if (other.get() != &s && other->controlling && other->want == id) taken = true;
                if (taken) {
                    drop(s, {id, wire::Drop::Riding, 0, "another pilot has it; riding along"});
                } else if (auto no = pilot.refuse(id)) {
                    drop(s, {id, wire::Drop::Denied, 0, *no});
                    s.want = 0;
                } else if (auto wait = pilot.busy(id)) {
                    if (wait->reason != s.lastReason || t - s.lastDropSent > 1.0)
                        drop(s, {id, wire::Drop::Riding, wait->eta, wait->reason});
                } else {
                    const Handling hd = pilot.take(id);
                    s.controlling = true;
                    s.controlAt = t; // grace: the pilot's first controls are on their way
                    wire::DropState ds{id, wire::Drop::Control, 0, "you have control"};
                    ds.model = static_cast<std::uint8_t>(hd.model);
                    ds.minSpeed = hd.minSpeed;
                    ds.maxSpeed = hd.maxSpeed;
                    ds.yawRate = hd.yawRate;
                    ds.climbRate = hd.climbRate;
                    ds.accel = hd.accel;
                    drop(s, ds);
                }
            } else {
                const double age = t - s.controlAt;
                if (age > options_.releaseTimeout) {
                    pilot.give(id);
                    drop(s, {id, wire::Drop::Released, 0, "pilot link lost; autonomy resumed"});
                    s.want = 0;
                    s.controlling = false;
                } else {
                    pilot.drive(id, s.control, age <= options_.controlTimeout, dt);
                }
            }
        }
        if (s.gone && s.want == 0)
            it = sessions_.erase(it);
        else
            ++it;
    }
}

// ---------------------------------------------------------------------------------------------
// Network thread

void Host::sendReliable(Session& s, wire::Type type, const std::vector<std::uint8_t>& payload, double t) {
    if (s.out.inFlight() >= kReliableWindow) {
        s.queued.emplace_back(type, payload);
        return;
    }
    const std::uint32_t seq = s.out.nextSeq();
    wire::Writer w;
    wire::writeHeader(w, {type, static_cast<std::uint8_t>(wire::kReliable | wire::kTagged), s.id, seq});
    w.bytes(payload);
    auto bytes = std::move(w.data());
    wire::sign(bytes, s.key);
    socket_->send(s.addr, bytes);
    s.out.sent(seq, std::move(bytes), t);
}

void Host::loop() {
    while (running_) {
        if (auto d = socket_->receive(2)) {
            handle(*d, net::now());
            // Drain whatever else has arrived before doing timed work.
            for (int i = 0; i < 64; ++i) {
                auto more = socket_->receive(0);
                if (!more) break;
                handle(*more, net::now());
            }
        }
        const double t = net::now();
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (auto& [addr, s] : sessions_) {
            if (!s->live) continue;
            for (auto& p : s->out.due(t)) socket_->send(addr, p);
            while (!s->queued.empty() && s->out.inFlight() < kReliableWindow) {
                auto [type, payload] = std::move(s->queued.front());
                s->queued.pop_front();
                sendReliable(*s, type, payload, t);
            }
            if (t - s->lastHeard > kSessionTimeout) s->gone = true;
        }
        // Sessions with nothing to hand back are removed here; a pilot's are removed by service().
        std::erase_if(sessions_, [](const auto& kv) { return kv.second->gone && kv.second->want == 0; });
        if (t - lastSnapshot_ >= 1.0 / std::max<double>(options_.snapshotHz, 1)) {
            lastSnapshot_ = t;
            broadcastFrame(t);
        }
    }
}

void Host::broadcastFrame(double t) {
    if (!haveFrame_) return;
    // Present the frame at the host's estimate of simulation time now.
    const double hostTime = frameTime_ + (t - frameWall_) * rate_;
    std::vector<std::vector<std::uint8_t>> payloads;
    {
        const std::size_t per = wire::kEntitiesPerPacket;
        const std::size_t parts = std::max<std::size_t>(1, (entities_.size() + per - 1) / per);
        for (std::size_t p = 0; p < parts; ++p) {
            wire::Snapshot snap;
            snap.hostTime = frameTime_;
            snap.rate = static_cast<float>(rate_);
            snap.tick = tick_;
            snap.part = static_cast<std::uint8_t>(p);
            snap.parts = static_cast<std::uint8_t>(parts);
            const auto b = entities_.begin() + static_cast<std::ptrdiff_t>(p * per);
            const auto e = entities_.begin() + static_cast<std::ptrdiff_t>(std::min(entities_.size(), (p + 1) * per));
            snap.entities.assign(b, e);
            wire::Writer w;
            wire::writeHeader(w, {wire::Type::Snapshot, wire::kTagged, 0, tick_});
            wire::encode(w, snap);
            payloads.push_back(std::move(w.data()));
        }
        const std::size_t perE = wire::kEffectsPerPacket;
        const std::size_t eparts = std::max<std::size_t>(1, (effects_.size() + perE - 1) / perE);
        for (std::size_t p = 0; p < eparts; ++p) {
            wire::Effects fx;
            fx.hostTime = frameTime_;
            fx.part = static_cast<std::uint8_t>(p);
            fx.parts = static_cast<std::uint8_t>(eparts);
            const auto b = effects_.begin() + static_cast<std::ptrdiff_t>(p * perE);
            const auto e = effects_.begin() + static_cast<std::ptrdiff_t>(std::min(effects_.size(), (p + 1) * perE));
            fx.effects.assign(b, e);
            wire::Writer w;
            wire::writeHeader(w, {wire::Type::Effects, wire::kTagged, 0, tick_});
            wire::encode(w, fx);
            payloads.push_back(std::move(w.data()));
        }
        if (t - lastStatus_ > 0.25 && !status_.empty()) {
            lastStatus_ = t;
            wire::Status st{hostTime, status_};
            wire::Writer w;
            wire::writeHeader(w, {wire::Type::Status, wire::kTagged, 0, tick_});
            wire::encode(w, st);
            payloads.push_back(std::move(w.data()));
        }
        if (t - lastInfo_ > 0.1 && !info_.empty()) {
            lastInfo_ = t;
            wire::Info in;
            std::size_t bytes = 1;
            auto it = info_.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(infoCursor_ % info_.size()));
            for (std::size_t n = 0; n < info_.size(); ++n) {
                const std::size_t need = 4 + 2 + it->second.name.size() + it->second.task.size();
                if (bytes + need > wire::kMaxPayload) break;
                in.items.push_back(it->second);
                bytes += need;
                ++infoCursor_;
                if (++it == info_.end()) it = info_.begin();
            }
            wire::Writer w;
            wire::writeHeader(w, {wire::Type::Info, wire::kTagged, 0, tick_});
            wire::encode(w, in);
            payloads.push_back(std::move(w.data()));
        }
    }
    for (auto& [addr, s] : sessions_) {
        if (!s->live) continue;
        for (const auto& p : payloads) {
            auto bytes = p;
            // Stamp the session id into the header (bytes 8..11) before signing.
            for (int i = 0; i < 4; ++i) bytes[8 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(s->id >> (8 * i));
            wire::sign(bytes, s->key);
            socket_->send(addr, bytes);
        }
    }
}

void Host::handle(const net::Datagram& d, double t) {
    wire::Reader r(d.bytes);
    const auto h = wire::readHeader(r);
    if (!h) return;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto payload = std::span(d.bytes).subspan(wire::kHeaderBytes, d.bytes.size() - wire::kHeaderBytes - wire::kTagBytes);
    wire::Reader body(payload);

    // Before a session: discovery and the pairing handshake.
    if (h->type == wire::Type::Discover) {
        if (!wire::verify(d.bytes, kNoKey)) return;
        wire::Beacon b{options_.name,  options_.title,
                       port_,          static_cast<std::uint16_t>(std::min<std::size_t>(entities_.size(), 65535)),
                       !code_.empty(), options_.environment};
        socket_->send(d.from, plain(wire::Type::Beacon, b));
        return;
    }
    if (h->type == wire::Type::Hello) {
        if (!wire::verify(d.bytes, kNoKey)) return;
        const auto f = failures_.find(d.from);
        if (f != failures_.end() && f->second.first >= kMaxFailures && t - f->second.second < kFailureWindow) return;
        const auto hello = wire::decode<wire::Hello>(body);
        if (!hello) return;
        auto& s = sessions_[d.from];
        if (s && s->controlling) pendingGive_.push_back(s->want); // a reconnect: hand the old pilot's node back
        s = std::make_unique<Session>();
        s->addr = d.from;
        s->client = hello->client;
        s->clientNonce = hello->nonce;
        crypto::randomBytes(s->hostNonce);
        s->lastHeard = t;
        socket_->send(d.from, plain(wire::Type::Challenge, wire::Challenge{s->hostNonce, !code_.empty()}));
        return;
    }
    const auto sit = sessions_.find(d.from);
    if (sit == sessions_.end()) return;
    Session& s = *sit->second;

    if (h->type == wire::Type::Auth) {
        if (!wire::verify(d.bytes, kNoKey)) return;
        const auto auth = wire::decode<wire::Auth>(body);
        if (!auth) return;
        const auto expect = wire::authProof(pairKey_, s.clientNonce, s.hostNonce);
        if (!crypto::equal(expect, auth->proof)) {
            auto& f = failures_[d.from];
            if (t - f.second > kFailureWindow) f = {0, t};
            ++f.first;
            socket_->send(d.from, plain(wire::Type::Denied, wire::Order{"pairing code does not match"}));
            sessions_.erase(sit);
            return;
        }
        failures_.erase(d.from);
        if (!s.live) {
            s.id = nextSession_++;
            s.key = wire::sessionKey(pairKey_, s.clientNonce, s.hostNonce);
            s.live = true;
        }
        s.lastHeard = t;
        wire::World w;
        w.session = s.id;
        w.hostTime = frameTime_ + (t - frameWall_) * rate_;
        w.snapshotHz = options_.snapshotHz;
        w.environment = options_.environment;
        w.title = options_.title;
        w.subtitle = options_.subtitle;
        w.originLat = options_.originLat;
        w.originLon = options_.originLon;
        w.groundZ = options_.groundZ;
        w.radius = options_.radius;
        w.terrainBytes = static_cast<std::uint32_t>(terrain_.size());
        w.help = options_.help;
        socket_->send(d.from, net::packet(wire::Type::Welcome, s.id, 0, 0, w, s.key));
        for (std::size_t off = 0; off < terrain_.size(); off += wire::kTerrainChunk) {
            wire::TerrainChunk c;
            c.offset = static_cast<std::uint32_t>(off);
            c.total = static_cast<std::uint32_t>(terrain_.size());
            c.bytes.assign(terrain_.begin() + static_cast<std::ptrdiff_t>(off),
                           terrain_.begin() + static_cast<std::ptrdiff_t>(std::min(terrain_.size(), off + wire::kTerrainChunk)));
            sendReliable(s, wire::Type::Terrain, c, t);
        }
        return;
    }

    // Everything else belongs to a live, authenticated session.
    if (!s.live || h->session != s.id || !wire::verify(d.bytes, s.key)) return;
    s.lastHeard = t;

    if (h->flags & wire::kReliable) {
        auto ready = s.in.accept(h->seq, h->type, payload);
        socket_->send(d.from, net::packet(wire::Type::Ack, s.id, 0, 0, s.in.ack(), s.key));
        for (auto& [type, bytes] : ready) {
            wire::Reader rr(bytes);
            if (type == wire::Type::DropIn) {
                if (const auto m = wire::decode<wire::DropIn>(rr)) {
                    s.want = m->entity;
                    s.controlling = false;
                    s.releaseRequested = false;
                    s.lastReason.clear();
                }
            } else if (type == wire::Type::Release) {
                if (s.want) s.releaseRequested = true;
            } else if (type == wire::Type::Order) {
                if (const auto m = wire::decode<wire::Order>(rr)) orders_.push_back(m->text);
            }
        }
        return;
    }
    switch (h->type) {
    case wire::Type::Ack:
        if (const auto a = wire::decode<wire::Ack>(body)) s.out.acked(*a, t);
        break;
    case wire::Type::Control:
        if (const auto c = wire::decode<wire::Control>(body))
            if (c->seq > s.control.seq || s.controlAt < 0) {
                s.control = *c;
                s.controlAt = t;
            }
        break;
    case wire::Type::Ping:
        if (const auto p = wire::decode<wire::Ping>(body))
            socket_->send(d.from, net::packet(wire::Type::Pong, s.id, 0, 0,
                                              wire::Pong{p->id, p->clientTime, frameTime_ + (t - frameWall_) * rate_}, s.key));
        break;
    case wire::Type::Bye: s.gone = true; break;
    default: break;
    }
}

} // namespace glyder
