#pragma once

// The host side of glyder: what a Gygax simulation (or `gygax serve` with real devices) embeds to
// put its world into VR and let a pilot drop into its nodes.
//
//   glyder::Host host({.name = "ohflock", .title = "OHFLOCK"});
//   host.start(&error);                       // prints nothing; show host.pairingCode() to the user
//   each simulation step:
//       host.publish(t, entities, effects);   // the world as it is now
//       host.service(pilot, dt);              // drop-in requests and pilots' controls, on this thread
//
// Networking runs on its own thread; everything that touches the simulation (the Pilot callbacks)
// runs inside service(), on the simulation's thread, so a simulation needs no locking of its own.
// Pilot callbacks may call back into the Host (to announce a takeover, say).

#include <atomic>
#include <cstdint>
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

struct HostOptions {
    std::string name{"gygax"}; // shown in the lobby
    std::string title, subtitle, help;
    std::string bind{"0.0.0.0"};
    std::uint16_t port{wire::kDefaultPort};
    // The pairing secret. Empty: a random six-digit code, unless bound to loopback or `open`.
    std::string secret;
    bool open{false}; // no pairing (a trusted, private network only)
    std::uint16_t snapshotHz{30};
    std::uint8_t environment{0};
    double originLat{0}, originLon{0};
    float groundZ{0}, radius{10000};
    double controlTimeout{0.25}; // controls older than this are stale: the node holds
    double releaseTimeout{1.5};  // a pilot silent this long loses control: autonomy resumes
};

// What a simulation implements so a pilot can drop into its nodes. All calls happen inside
// Host::service(), on the simulation's own thread.
class Pilot {
public:
    virtual ~Pilot() = default;
    // nullopt if `id` can be driven at all, otherwise why not.
    virtual std::optional<std::string> refuse(std::uint32_t id) = 0;
    struct Wait {
        std::string reason; // what it is finishing first
        float eta{0};       // seconds, if known
    };
    // nullopt at a good stopping point, otherwise what it is in the middle of.
    virtual std::optional<Wait> busy(std::uint32_t id) = 0;
    // Autonomy off; drive() follows. Returns how the node handles so the pilot's client can
    // predict it (Model::None: no prediction, the client waits for the host's state).
    virtual Handling take(std::uint32_t id) = 0;
    // The newest controls each step. `fresh` is false when they are older than the control
    // timeout: hold position or attitude safely until they resume.
    virtual void drive(std::uint32_t id, const wire::Control& controls, bool fresh, double dt) = 0;
    virtual void give(std::uint32_t id) = 0; // autonomy back on
    virtual bool exists(std::uint32_t id) = 0;
};

class Host {
public:
    explicit Host(HostOptions options);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    bool start(std::string* error);
    void stop();
    [[nodiscard]] const std::string& pairingCode() const { return code_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] std::size_t clients() const;
    [[nodiscard]] const HostOptions& options() const { return options_; }

    // The simulation's thread:
    void setTerrain(const wire::Heightfield& terrain);
    void publish(double simTime, std::vector<wire::EntityState> entities, std::vector<wire::Effect> effects = {});
    void info(std::uint32_t id, std::string name, std::string task);
    void status(std::vector<wire::StatusItem> items);
    void event(wire::EventKind kind, std::string text, double time);
    void service(Pilot& pilot, double dt);
    [[nodiscard]] std::vector<std::string> takeOrders();
    [[nodiscard]] bool driven(std::uint32_t id) const;

private:
    struct Session;
    void loop();
    void handle(const net::Datagram& d, double t);
    void sendReliable(Session& s, wire::Type type, const std::vector<std::uint8_t>& payload, double t);
    template <class M> void sendReliable(Session& s, wire::Type type, const M& m, double t) {
        wire::Writer w;
        wire::encode(w, m);
        sendReliable(s, type, w.data(), t);
    }
    void broadcastFrame(double t);
    void drop(Session& s, const wire::DropState& d);

    HostOptions options_;
    std::string code_;
    std::array<std::uint8_t, 32> pairKey_{};
    std::unique_ptr<net::Socket> socket_;
    std::uint16_t port_{0};
    std::jthread thread_;
    std::atomic<bool> running_{false};

    // Recursive: a Pilot callback (inside service()) may call event(), info() or status().
    mutable std::recursive_mutex mutex_;
    std::map<net::Address, std::unique_ptr<Session>> sessions_;
    std::map<net::Address, std::pair<int, double>> failures_; // failed pairings: count, first time
    std::uint32_t nextSession_{1};
    // The frame the simulation last published.
    double frameTime_{0}, frameWall_{0}, rate_{1};
    std::uint32_t tick_{0};
    bool haveFrame_{false};
    std::vector<wire::EntityState> entities_;
    std::vector<wire::Effect> effects_;
    std::map<std::uint32_t, wire::InfoItem> info_;
    std::vector<wire::StatusItem> status_;
    std::vector<std::uint8_t> terrain_;
    std::vector<std::string> orders_;
    std::vector<std::uint32_t> pendingGive_;
    std::size_t infoCursor_{0};
    double lastSnapshot_{0}, lastStatus_{0}, lastInfo_{0};
};

} // namespace glyder
