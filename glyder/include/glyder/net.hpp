#pragma once

// UDP sockets, addresses, a monotonic clock and the reliable channel, shared by the host and the
// client. POSIX only (Linux, Android, macOS).

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glyder/wire.hpp>

namespace glyder::net {

// Seconds on a monotonic clock.
[[nodiscard]] double now();

struct Address {
    std::uint32_t ip{0}; // host byte order
    std::uint16_t port{0};
    [[nodiscard]] std::string str() const;
    [[nodiscard]] bool loopback() const { return (ip >> 24) == 127; }
    friend bool operator==(const Address&, const Address&) = default;
    friend auto operator<=>(const Address&, const Address&) = default;
    // "host", "host:port", "a.b.c.d:port"; the port defaults to `port`.
    [[nodiscard]] static std::optional<Address> parse(const std::string& s, std::uint16_t port = wire::kDefaultPort);
    [[nodiscard]] static Address broadcast(std::uint16_t port) { return {0xFFFFFFFFu, port}; }
};

struct Datagram {
    Address from;
    std::vector<std::uint8_t> bytes;
};

class Socket {
public:
    // Bind to `host:port` (port 0 picks one). Broadcast enables sending to 255.255.255.255.
    static std::unique_ptr<Socket> open(const std::string& host, std::uint16_t port, bool broadcast, std::string* error);
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    bool send(const Address& to, std::span<const std::uint8_t> bytes);
    // Wait up to `timeoutMs` for one datagram.
    [[nodiscard]] std::optional<Datagram> receive(int timeoutMs);
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    explicit Socket(int fd, std::uint16_t port) : fd_(fd), port_(port) {}
    int fd_;
    std::uint16_t port_;
};

// The addresses of this machine's IPv4 interfaces that are up and not loopback.
[[nodiscard]] std::vector<Address> localAddresses();

// Reliable, in-order delivery over unreliable datagrams: sequence numbers, cumulative acks with a
// 32-packet selective mask, and resends on a timeout tracking the measured round trip.
class ReliableOut {
public:
    // Queue a packet (header already carries `seq`); returns that sequence number.
    std::uint32_t nextSeq() { return next_++; }
    void sent(std::uint32_t seq, std::vector<std::uint8_t> packet, double t);
    void acked(const wire::Ack& ack, double t);
    // Packets whose resend timer has expired; their timers restart.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> due(double t);
    [[nodiscard]] std::size_t inFlight() const { return pending_.size(); }
    [[nodiscard]] double rtt() const { return rtt_; }

private:
    struct Item {
        std::vector<std::uint8_t> packet;
        double firstSent, lastSent;
        int sends;
    };
    std::uint32_t next_{1};
    std::map<std::uint32_t, Item> pending_;
    double rtt_{0.05};
};

class ReliableIn {
public:
    // Accept a reliable packet's payload; returns the payloads now deliverable in order (possibly
    // none, possibly several). Duplicates are dropped.
    std::vector<std::pair<wire::Type, std::vector<std::uint8_t>>> accept(std::uint32_t seq, wire::Type type,
                                                                         std::span<const std::uint8_t> payload);
    [[nodiscard]] wire::Ack ack() const;

private:
    std::uint32_t delivered_{0};
    std::map<std::uint32_t, std::pair<wire::Type, std::vector<std::uint8_t>>> held_;
};

// Build a complete packet: header, payload, tag.
template <class M>
[[nodiscard]] std::vector<std::uint8_t> packet(wire::Type type, std::uint32_t session, std::uint32_t seq, std::uint8_t flags,
                                               const M& message, std::span<const std::uint8_t> key) {
    wire::Writer w;
    wire::writeHeader(w, {type, static_cast<std::uint8_t>(flags | wire::kTagged), session, seq});
    wire::encode(w, message);
    auto bytes = std::move(w.data());
    wire::sign(bytes, key);
    return bytes;
}

} // namespace glyder::net
