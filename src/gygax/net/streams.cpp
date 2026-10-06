#include <gygax/transport/streams.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

#include <gygax/core/log.hpp>
#include <gygax/core/posix.hpp>

namespace gygax::comm {

namespace {

constexpr std::size_t kUdpReceiveBufferCapacityBytes = 65536;
constexpr std::size_t kTcpReceiveChunkBytes = 4096;
constexpr int kTcpTransmitRetryAttempts = 2;

struct Endpoint {
    std::string host;
    std::string port;
};

std::optional<Endpoint> splitEndpoint(const std::string& destination) {
    const auto colon = destination.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == destination.size()) return std::nullopt;
    Endpoint ep{destination.substr(0, colon), destination.substr(colon + 1)};
    if (ep.host.front() == '[' && ep.host.back() == ']') ep.host = ep.host.substr(1, ep.host.size() - 2);
    return ep;
}

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class UdpStream final : public Stream {
public:
    UdpStream(std::uint16_t bindPort, const std::string& bindHost) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) {
            log::error("udp", "socket failed: {}", std::strerror(errno));
            return;
        }
        setNonBlocking(fd_);
        if (bindPort != 0) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(bindPort);
            if (::inet_pton(AF_INET, bindHost.c_str(), &addr.sin_addr) != 1 ||
                ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                log::error("udp", "cannot bind {}:{}: {}", bindHost, bindPort, std::strerror(errno));
                ::close(fd_);
                fd_ = -1;
            }
        }
    }

    ~UdpStream() override {
        if (fd_ >= 0) ::close(fd_);
    }

    bool transmit(const std::string& destination, const std::string& message) override {
        if (fd_ < 0) return false;
        auto ep = splitEndpoint(destination);
        if (!ep) return false;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(ep->host.c_str(), ep->port.c_str(), &hints, &res) != 0) return false;
        const ssize_t n = ::sendto(fd_, message.data(), message.size(), 0, res->ai_addr, res->ai_addrlen);
        ::freeaddrinfo(res);
        return n == static_cast<ssize_t>(message.size());
    }

    std::optional<std::string> receive() override {
        if (fd_ < 0) return std::nullopt;
        std::string buffer(kUdpReceiveBufferCapacityBytes, '\0');
        const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
        if (n < 0) return std::nullopt;
        buffer.resize(static_cast<std::size_t>(n));
        return buffer;
    }

    [[nodiscard]] std::string name() const override { return "udp"; }

private:
    int fd_ = -1;
};

class TcpLineStream final : public Stream {
public:
    ~TcpLineStream() override {
        for (auto& [dest, conn] : connections_) ::close(conn.fd);
    }

    bool transmit(const std::string& destination, const std::string& message) override {
        std::lock_guard lock(mutex_);
        for (int attempt = 0; attempt < kTcpTransmitRetryAttempts; ++attempt) {
            auto it = connections_.find(destination);
            if (it == connections_.end()) {
                const int fd = connect(destination);
                if (fd < 0) return false;
                it = connections_.emplace(destination, Connection{fd, {}}).first;
            }
            const std::string line = message + "\n";
            std::size_t sent = 0;
            bool failed = false;
            while (sent < line.size()) {
                const ssize_t n = ::send(it->second.fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
                if (n <= 0) {
                    failed = true;
                    break;
                }
                sent += static_cast<std::size_t>(n);
            }
            if (!failed) return true;
            ::close(it->second.fd);
            connections_.erase(it);
        }
        return false;
    }

    std::optional<std::string> receive() override {
        std::lock_guard lock(mutex_);
        if (!inbox_.empty()) {
            auto line = std::move(inbox_.front());
            inbox_.pop_front();
            return line;
        }
        for (auto& [dest, conn] : connections_) {
            char buf[kTcpReceiveChunkBytes];
            while (true) {
                const ssize_t n = ::recv(conn.fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                conn.pending.append(buf, static_cast<std::size_t>(n));
            }
            std::size_t nl;
            while ((nl = conn.pending.find('\n')) != std::string::npos) {
                std::string line = conn.pending.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                inbox_.push_back(std::move(line));
                conn.pending.erase(0, nl + 1);
            }
        }
        if (inbox_.empty()) return std::nullopt;
        auto line = std::move(inbox_.front());
        inbox_.pop_front();
        return line;
    }

    [[nodiscard]] std::string name() const override { return "tcp"; }

private:
    struct Connection {
        int fd;
        std::string pending;
    };

    static int connect(const std::string& destination) {
        auto ep = splitEndpoint(destination);
        if (!ep) return -1;
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(ep->host.c_str(), ep->port.c_str(), &hints, &res) != 0) return -1;
        int fd = -1;
        for (addrinfo* ai = res; ai != nullptr && fd < 0; ai = ai->ai_next) {
            const int s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s < 0) continue;
            if (::connect(s, ai->ai_addr, ai->ai_addrlen) == 0) {
                suppressSigpipe(s);
                fd = s;
            } else {
                ::close(s);
            }
        }
        ::freeaddrinfo(res);
        if (fd >= 0) setNonBlocking(fd);
        return fd;
    }

    std::mutex mutex_;
    std::map<std::string, Connection> connections_;
    std::deque<std::string> inbox_;
};

}

std::unique_ptr<Stream> CreateUdpStream(std::uint16_t bindPort, const std::string& bindHost) {
    return std::make_unique<UdpStream>(bindPort, bindHost);
}

std::unique_ptr<Stream> CreateTcpLineStream() {
    return std::make_unique<TcpLineStream>();
}

}
