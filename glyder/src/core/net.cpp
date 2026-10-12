#include <glyder/net.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace glyder::net {

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Address::str() const {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xFF) + "." + std::to_string((ip >> 8) & 0xFF) + "." +
           std::to_string(ip & 0xFF) + ":" + std::to_string(port);
}

std::optional<Address> Address::parse(const std::string& s, std::uint16_t port) {
    std::string host = s;
    if (const auto colon = s.rfind(':'); colon != std::string::npos) {
        host = s.substr(0, colon);
        const long p = std::strtol(s.c_str() + colon + 1, nullptr, 10);
        if (p <= 0 || p > 65535) return std::nullopt;
        port = static_cast<std::uint16_t>(p);
    }
    if (host.empty() || host == "localhost") host = "127.0.0.1";
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) != 1) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return std::nullopt;
        a = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    return Address{ntohl(a.s_addr), port};
}

std::unique_ptr<Socket> Socket::open(const std::string& host, std::uint16_t port, bool broadcast, std::string* error) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error) *error = std::string("socket: ") + std::strerror(errno);
        return nullptr;
    }
    // No SO_REUSEADDR: on Linux it lets two UDP hosts share a port and split its traffic silently.
    // A second host on the same port should fail loudly instead.
    const int one = 1;
    if (broadcast) ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    // Latency over throughput: small buffers would drop under bursts, so keep them generous.
    const int buf = 1 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
#ifdef IPTOS_LOWDELAY
    const int tos = IPTOS_LOWDELAY;
    ::setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
#endif
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") {
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        const auto a = Address::parse(host, port);
        if (!a) {
            ::close(fd);
            if (error) *error = "cannot resolve " + host;
            return nullptr;
        }
        sa.sin_addr.s_addr = htonl(a->ip);
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        if (error) *error = "bind " + host + ":" + std::to_string(port) + ": " + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    socklen_t len = sizeof sa;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len);
    return std::unique_ptr<Socket>(new Socket(fd, ntohs(sa.sin_port)));
}

Socket::~Socket() {
    ::close(fd_);
}

bool Socket::send(const Address& to, std::span<const std::uint8_t> bytes) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(to.port);
    sa.sin_addr.s_addr = htonl(to.ip);
    return ::sendto(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&sa), sizeof sa) ==
           static_cast<ssize_t>(bytes.size());
}

std::optional<Datagram> Socket::receive(int timeoutMs) {
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, timeoutMs) <= 0 || !(p.revents & POLLIN)) return std::nullopt;
    std::uint8_t buf[2048];
    sockaddr_in sa{};
    socklen_t len = sizeof sa;
    const ssize_t n = ::recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&sa), &len);
    if (n <= 0) return std::nullopt;
    Datagram d;
    d.from = {ntohl(sa.sin_addr.s_addr), ntohs(sa.sin_port)};
    d.bytes.assign(buf, buf + n);
    return d;
}

std::vector<Address> localAddresses() {
    std::vector<Address> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        out.push_back({ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr), 0});
    }
    ::freeifaddrs(list);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Reliable channel

void ReliableOut::sent(std::uint32_t seq, std::vector<std::uint8_t> packet, double t) {
    pending_[seq] = {std::move(packet), t, t, 1};
}

void ReliableOut::acked(const wire::Ack& ack, double t) {
    auto settle = [&](std::uint32_t seq) {
        const auto it = pending_.find(seq);
        if (it == pending_.end()) return;
        if (it->second.sends == 1) rtt_ = rtt_ * 0.875 + (t - it->second.firstSent) * 0.125; // Karn: only unambiguous samples
        pending_.erase(it);
    };
    while (!pending_.empty() && pending_.begin()->first <= ack.cumulative) settle(pending_.begin()->first);
    for (std::uint32_t i = 0; i < 32; ++i)
        if (ack.mask & (1u << i)) settle(ack.cumulative + 1 + i);
}

std::vector<std::vector<std::uint8_t>> ReliableOut::due(double t) {
    std::vector<std::vector<std::uint8_t>> out;
    const double rto = std::clamp(rtt_ * 2.0 + 0.02, 0.04, 1.0);
    for (auto& [seq, item] : pending_)
        if (t - item.lastSent >= rto * static_cast<double>(std::min(item.sends, 4))) {
            out.push_back(item.packet);
            item.lastSent = t;
            ++item.sends;
        }
    return out;
}

std::vector<std::pair<wire::Type, std::vector<std::uint8_t>>> ReliableIn::accept(std::uint32_t seq, wire::Type type,
                                                                                 std::span<const std::uint8_t> payload) {
    std::vector<std::pair<wire::Type, std::vector<std::uint8_t>>> out;
    if (seq <= delivered_ || held_.count(seq)) return out;
    held_[seq] = {type, std::vector<std::uint8_t>(payload.begin(), payload.end())};
    while (!held_.empty() && held_.begin()->first == delivered_ + 1) {
        out.push_back(std::move(held_.begin()->second));
        held_.erase(held_.begin());
        ++delivered_;
    }
    return out;
}

wire::Ack ReliableIn::ack() const {
    wire::Ack a{delivered_, 0};
    for (const auto& [seq, p] : held_)
        if (seq > delivered_ && seq - delivered_ - 1 < 32) a.mask |= 1u << (seq - delivered_ - 1);
    return a;
}

} // namespace glyder::net
