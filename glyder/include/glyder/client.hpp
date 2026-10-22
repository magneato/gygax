#pragma once

// The client side of glyder: finds hosts on the LAN, pairs, and keeps a live model of the host's
// world for the renderer. Networking runs on its own thread; the app reads a consistent view.

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <glyder/handling.hpp>
#include <glyder/net.hpp>
#include <glyder/wire.hpp>

namespace glyder {

struct Found {
    net::Address addr;
    wire::Beacon beacon;
    double seen{0};
};

class Client {
public:
    explicit Client(std::string name);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool open(std::string* error);

    // Discovery: while on, broadcasts once a second and collects beacons (and asks the explicit
    // addresses given, for networks that block broadcast).
    void discover(bool on, std::vector<net::Address> also = {});
    [[nodiscard]] std::vector<Found> found() const;

    enum class State { Idle, Connecting, Connected, Failed };
    void connect(net::Address host, std::string secret);
    void disconnect();
    [[nodiscard]] State state() const;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] std::optional<wire::World> world() const;
    // Bumps each time the host switches to another world mid-session (a scene change).
    [[nodiscard]] std::uint64_t worldRevision() const;
    [[nodiscard]] net::Address host() const;

    // Terrain arrives in reliable chunks after Welcome.
    [[nodiscard]] float terrainProgress() const;
    [[nodiscard]] std::optional<wire::Heightfield> takeTerrain();

    // The world at client time `t` (net::now()): every entity dead-reckoned to the host's present
    // with snapshot corrections blended out smoothly.
    void sample(double t, std::vector<wire::EntityState>& out) const;
    [[nodiscard]] double hostTimeAt(double t) const;
    [[nodiscard]] std::vector<wire::Effect> effects() const;
    [[nodiscard]] std::optional<wire::InfoItem> info(std::uint32_t id) const;
    [[nodiscard]] wire::Status status() const;
    [[nodiscard]] std::vector<wire::Event> takeEvents();

    // Drop-in.
    void dropIn(std::uint32_t entity);
    void release();
    void control(const wire::Control& c);
    void order(const std::string& text);
    [[nodiscard]] std::vector<wire::DropState> takeDropStates();

    [[nodiscard]] double rtt() const;
    [[nodiscard]] double sinceHeard(double t) const;
    [[nodiscard]] std::uint64_t packetsIn() const { return packetsIn_; }

private:
    struct Track {
        wire::EntityState last; // newest authoritative state
        double hostTime{0};     // its host time
        double arrived{0};      // client time it arrived
        Vec3 error;             // display offset being blended out
        double errorAt{0};
        Quat shown; // orientation shown, slerped toward the newest
        double shownAt{0};
    };

    void loop();
    void handle(const net::Datagram& d, double t);
    void sendReliable(wire::Type type, const std::vector<std::uint8_t>& payload);
    template <class M> void sendReliable(wire::Type type, const M& m) {
        wire::Writer w;
        wire::encode(w, m);
        sendReliable(type, w.data());
    }
    template <class M> void send(wire::Type type, const M& m);

    std::string name_;
    std::unique_ptr<net::Socket> socket_;
    std::jthread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> packetsIn_{0};

    mutable std::mutex mutex_;
    bool discovering_{false};
    std::vector<net::Address> also_;
    double lastDiscover_{0};
    std::map<net::Address, Found> found_;

    State state_{State::Idle};
    std::string error_;
    net::Address host_;
    std::string secret_;
    std::array<std::uint8_t, 16> nonce_{};
    std::array<std::uint8_t, 32> key_{};
    std::uint32_t session_{0};
    double lastHello_{0}, lastHeard_{0}, lastPing_{0};
    std::uint32_t pingId_{0};
    double rtt_{0.0};
    std::optional<wire::World> world_;
    std::uint64_t worldRevision_{0};
    net::ReliableOut out_;
    net::ReliableIn in_;

    // Host clock: hostTime(t) = base_ + (t - baseAt_) * rate_.
    double base_{0}, baseAt_{0}, rate_{1};
    bool clockSet_{false};

    std::map<std::uint32_t, Track> tracks_;
    double fxTime_{-1};
    std::vector<wire::Effect> fx_;
    std::map<std::uint32_t, wire::InfoItem> info_;
    wire::Status status_;
    std::deque<wire::Event> events_;
    std::deque<wire::DropState> drops_;
    std::vector<std::uint8_t> terrainBytes_;
    std::vector<bool> terrainHave_;
    std::size_t terrainGot_{0};
    std::optional<wire::Heightfield> terrain_;
    std::uint32_t controlSeq_{0};
};

} // namespace glyder
