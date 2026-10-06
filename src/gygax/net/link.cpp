#include <gygax/net/link.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>

#include <gygax/core/charconv.hpp>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>

#include <gygax/core/posix.hpp>
#include <gygax/transport/serial.hpp>

namespace gygax::net {

namespace {

constexpr std::size_t kLinkReadBufferSize = 4096;
constexpr int kSocketWritePollTimeoutMilliseconds = 2000;
constexpr std::uint32_t kMaximumNetworkPort = 65535;
constexpr std::chrono::seconds kDefaultTcpConnectTimeout{5};
constexpr const char* kWildcardIpv4Address = "0.0.0.0";

class MemoryLink final : public ByteLink {
public:
    struct Shared {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<std::vector<std::uint8_t>> queue[2];
    };

    MemoryLink(std::shared_ptr<Shared> shared, int side) : shared_(std::move(shared)), side_(side) {}

    IOResult write(std::span<const std::uint8_t> data) override {
        {
            std::lock_guard lock(shared_->mutex);
            shared_->queue[1 - side_].emplace_back(data.begin(), data.end());
        }
        shared_->ready.notify_all();
        return 0;
    }

    IOResult read(std::vector<std::uint8_t>& out, std::chrono::milliseconds timeout) override {
        std::unique_lock lock(shared_->mutex);
        if (!shared_->ready.wait_for(lock, timeout, [&] { return !shared_->queue[side_].empty(); })) return -ETIMEDOUT;
        out = std::move(shared_->queue[side_].front());
        shared_->queue[side_].pop_front();
        return 0;
    }

    std::string describe() const override { return "memory"; }

private:
    std::shared_ptr<Shared> shared_;
    int side_;
};

class FdLink : public ByteLink {
public:
    FdLink(int fd, std::string description) : fd_(fd), description_(std::move(description)) {}

    ~FdLink() override {
        if (fd_ >= 0) ::close(fd_);
    }

    IOResult read(std::vector<std::uint8_t>& out, std::chrono::milliseconds timeout) override {
        pollfd p{fd_, POLLIN, 0};
        const int rc = ::poll(&p, 1, static_cast<int>(timeout.count()));
        if (rc == 0) return -ETIMEDOUT;
        if (rc < 0) return -errno;
        std::uint8_t buf[kLinkReadBufferSize];
        const ssize_t n = receive(buf, sizeof(buf));
        if (n == 0) return -ECONNRESET;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? -ETIMEDOUT : -errno;
        out.assign(buf, buf + n);
        return 0;
    }

    std::string describe() const override { return description_; }

protected:
    virtual ssize_t receive(std::uint8_t* buf, std::size_t size) { return ::recv(fd_, buf, size, 0); }

    int fd_;
    std::string description_;
};

class UdpLink final : public FdLink {
public:
    UdpLink(int fd, std::string description, bool hasRemote, sockaddr_storage remote, socklen_t remoteLen)
        : FdLink(fd, std::move(description)), hasRemote_(hasRemote), remote_(remote), remoteLen_(remoteLen) {}

    IOResult write(std::span<const std::uint8_t> data) override {
        std::lock_guard lock(mutex_);
        if (!hasRemote_) return -ENOTCONN;
        const ssize_t n = ::sendto(fd_, data.data(), data.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&remote_), remoteLen_);
        return n == static_cast<ssize_t>(data.size()) ? 0 : -errno;
    }

protected:
    ssize_t receive(std::uint8_t* buf, std::size_t size) override {
        sockaddr_storage from{};
        socklen_t len = sizeof(from);
        const ssize_t n = ::recvfrom(fd_, buf, size, 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n > 0) {
            std::lock_guard lock(mutex_);
            if (!fixedRemote_) {
                remote_ = from;
                remoteLen_ = len;
                hasRemote_ = true;
            }
        }
        return n;
    }

public:
    void fixRemote() { fixedRemote_ = true; }

private:
    std::mutex mutex_;
    bool hasRemote_;
    bool fixedRemote_ = false;
    sockaddr_storage remote_;
    socklen_t remoteLen_;
};

class TcpLink final : public FdLink {
public:
    using FdLink::FdLink;

    IOResult write(std::span<const std::uint8_t> data) override {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd p{fd_, POLLOUT, 0};
                if (::poll(&p, 1, kSocketWritePollTimeoutMilliseconds) <= 0) return -ETIMEDOUT;
                continue;
            }
            return -errno;
        }
        return 0;
    }
};

class SerialLink final : public ByteLink {
public:
    SerialLink(std::unique_ptr<transport::SerialTransport> serial, transport::TransportHandle handle, std::string description)
        : serial_(std::move(serial)), handle_(handle), description_(std::move(description)) {}

    IOResult write(std::span<const std::uint8_t> data) override {
        std::size_t written = 0;
        std::size_t total = 0;
        while (total < data.size()) {
            const auto rc = serial_->write(handle_, data.data() + total, data.size() - total, written);
            total += written;
            if (rc != 0 && rc != -EAGAIN) return rc;
            if (rc == -EAGAIN && written == 0) return -EAGAIN;
        }
        return 0;
    }

    IOResult read(std::vector<std::uint8_t>& out, std::chrono::milliseconds timeout) override {
        if (const auto rc = serial_->waitReadable(handle_, 1, timeout); rc != 0) return rc;
        std::uint8_t buf[kLinkReadBufferSize];
        std::size_t n = 0;
        if (const auto rc = serial_->read(handle_, buf, sizeof(buf), n); rc != 0) return rc;
        out.assign(buf, buf + n);
        return 0;
    }

    std::string describe() const override { return description_; }

private:
    std::unique_ptr<transport::SerialTransport> serial_;
    transport::TransportHandle handle_;
    std::string description_;
};

bool resolve(const std::string& host, std::uint16_t port, int socktype, sockaddr_storage& out, socklen_t& len, int& family,
             std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* res = nullptr;
    const std::string portText = std::to_string(port);
    if (const int rc = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &res); rc != 0 || res == nullptr) {
        if (error != nullptr) *error = std::format("cannot resolve '{}': {}", host, ::gai_strerror(rc));
        return false;
    }
    std::memcpy(&out, res->ai_addr, res->ai_addrlen);
    len = res->ai_addrlen;
    family = res->ai_family;
    ::freeaddrinfo(res);
    return true;
}

std::optional<std::uint32_t> parseNumber(std::string_view text) {
    std::uint32_t v = 0;
    auto [ptr, ec] = gygax::fromChars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || ptr != text.data() + text.size()) return std::nullopt;
    return v;
}

bool splitHostPort(std::string_view text, std::string& host, std::uint16_t& port, std::string* error) {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        if (error != nullptr) *error = "expected host:port";
        return false;
    }
    host = std::string(text.substr(0, colon));
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    const auto p = parseNumber(text.substr(colon + 1));
    if (!p || *p == 0 || *p > kMaximumNetworkPort) {
        if (error != nullptr) *error = "invalid port";
        return false;
    }
    port = static_cast<std::uint16_t>(*p);
    return true;
}

} // namespace

namespace {

struct MemoryRegistry {
    std::mutex mutex;
    std::map<std::string, std::pair<std::shared_ptr<ByteLink>, std::shared_ptr<ByteLink>>> pairs;

    static MemoryRegistry& instance() {
        static MemoryRegistry r;
        return r;
    }
};

std::shared_ptr<ByteLink> takeMemorySide(const std::string& name, bool first) {
    auto& reg = MemoryRegistry::instance();
    std::lock_guard lock(reg.mutex);
    auto it = reg.pairs.find(name);
    if (it == reg.pairs.end()) it = reg.pairs.emplace(name, makeLinkPair()).first;
    auto& side = first ? it->second.first : it->second.second;
    return std::exchange(side, nullptr);
}

} // namespace

std::shared_ptr<ByteLink> memoryLinkPeer(const std::string& name) {
    return takeMemorySide(name, false);
}

void resetMemoryLinks() {
    auto& reg = MemoryRegistry::instance();
    std::lock_guard lock(reg.mutex);
    reg.pairs.clear();
}

std::pair<std::shared_ptr<ByteLink>, std::shared_ptr<ByteLink>> makeLinkPair() {
    auto shared = std::make_shared<MemoryLink::Shared>();
    return {std::make_shared<MemoryLink>(shared, 0), std::make_shared<MemoryLink>(shared, 1)};
}

std::shared_ptr<ByteLink> openUdpLink(const std::string& bindHost, std::uint16_t bindPort, const std::string& remoteHost,
                                      std::uint16_t remotePort, std::string* error) {
    sockaddr_storage local{};
    socklen_t localLen = 0;
    int family = AF_INET;
    if (!resolve(bindHost.empty() ? std::string(kWildcardIpv4Address) : bindHost, bindPort, SOCK_DGRAM, local, localLen, family, error))
        return nullptr;
    const int fd = gygax::openSocket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
        if (error != nullptr) *error = std::string("socket: ") + std::strerror(errno);
        return nullptr;
    }
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&local), localLen) != 0) {
        if (error != nullptr) *error = std::format("cannot bind udp {}:{}: {}", bindHost, bindPort, std::strerror(errno));
        ::close(fd);
        return nullptr;
    }
    sockaddr_storage remote{};
    socklen_t remoteLen = 0;
    const bool hasRemote = !remoteHost.empty() && remotePort != 0;
    if (hasRemote) {
        int rfamily = family;
        if (!resolve(remoteHost, remotePort, SOCK_DGRAM, remote, remoteLen, rfamily, error) || rfamily != family) {
            if (error != nullptr && error->empty()) *error = "remote address family differs from the local socket";
            ::close(fd);
            return nullptr;
        }
    }
    auto link = std::make_shared<UdpLink>(fd, std::format("udp:{}:{}", bindHost, bindPort), hasRemote, remote, remoteLen);
    if (hasRemote) link->fixRemote();
    return link;
}

std::shared_ptr<ByteLink> openTcpLink(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout, std::string* error) {
    sockaddr_storage addr{};
    socklen_t len = 0;
    int family = AF_INET;
    if (!resolve(host, port, SOCK_STREAM, addr, len, family, error)) return nullptr;
    const int fd = gygax::openSocket(family, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error != nullptr) *error = std::string("socket: ") + std::strerror(errno);
        return nullptr;
    }
    suppressSigpipe(fd);
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), len);
    int err = rc == 0 ? 0 : errno;
    if (rc != 0 && err == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, static_cast<int>(timeout.count())) > 0) {
            socklen_t elen = sizeof(err);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
        } else {
            err = ETIMEDOUT;
        }
    }
    if (err != 0) {
        if (error != nullptr) *error = std::format("cannot connect to {}:{}: {}", host, port, std::strerror(err));
        ::close(fd);
        return nullptr;
    }
    const int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    return std::make_shared<TcpLink>(fd, std::format("tcp:{}:{}", host, port));
}

std::shared_ptr<ByteLink> openSerialLink(const std::string& device, std::uint32_t baud, std::string* error) {
    auto serial = std::make_unique<transport::SerialTransport>();
    if (const auto rc = serial->initialize(); rc != 0) {
        if (error != nullptr) *error = transport::describeError(rc);
        return nullptr;
    }
    transport::TransportHandle handle = 0;
    if (const auto rc = serial->open(device, handle); rc != 0) {
        if (error != nullptr) *error = std::format("cannot open {}: {}", device, transport::describeError(rc));
        return nullptr;
    }
    using transport::BaudRate;
    BaudRate rate = BaudRate::Baud57600;
    switch (baud) {
    case 300: rate = BaudRate::Baud300; break;
    case 1200: rate = BaudRate::Baud1200; break;
    case 2400: rate = BaudRate::Baud2400; break;
    case 4800: rate = BaudRate::Baud4800; break;
    case 9600: rate = BaudRate::Baud9600; break;
    case 19200: rate = BaudRate::Baud19200; break;
    case 38400: rate = BaudRate::Baud38400; break;
    case 57600: rate = BaudRate::Baud57600; break;
    case 115200: rate = BaudRate::Baud115200; break;
    case 230400: rate = BaudRate::Baud230400; break;
    default:
        if (error != nullptr) *error = "unsupported baud rate";
        return nullptr;
    }
    if (const auto rc = serial->configure(handle, rate); rc != 0 && rc != -ENOTTY) {
        if (error != nullptr) *error = std::format("cannot configure {}: {}", device, transport::describeError(rc));
        return nullptr;
    }
    return std::make_shared<SerialLink>(std::move(serial), handle, std::format("serial:{}@{}", device, baud));
}

std::shared_ptr<ByteLink> openLink(std::string_view uri, std::string* error) {
    auto fail = [&](std::string message) -> std::shared_ptr<ByteLink> {
        if (error != nullptr) *error = std::move(message);
        return nullptr;
    };
    std::string_view query;
    if (const auto q = uri.find('?'); q != std::string_view::npos) {
        query = uri.substr(q + 1);
        uri = uri.substr(0, q);
    }
    auto param = [&](std::string_view key) -> std::optional<std::string> {
        std::string_view rest = query;
        while (!rest.empty()) {
            const auto amp = rest.find('&');
            const auto pair = rest.substr(0, amp);
            const auto eq = pair.find('=');
            if (pair.substr(0, eq) == key) return std::string(eq == std::string_view::npos ? "" : pair.substr(eq + 1));
            if (amp == std::string_view::npos) break;
            rest.remove_prefix(amp + 1);
        }
        return std::nullopt;
    };
    std::string parseError;
    if (uri.starts_with("udp://")) {
        std::string host;
        std::uint16_t port = 0;
        if (!splitHostPort(uri.substr(6), host, port, &parseError)) return fail("invalid udp uri: " + parseError);
        if (auto bind = param("bind")) {
            const auto bp = parseNumber(*bind);
            if (!bp || *bp > kMaximumNetworkPort) return fail("invalid bind port");
            return openUdpLink(std::string(kWildcardIpv4Address), static_cast<std::uint16_t>(*bp), host, port, error);
        }
        return openUdpLink(host, port, "", 0, error);
    }
    if (uri.starts_with("tcp://")) {
        std::string host;
        std::uint16_t port = 0;
        if (!splitHostPort(uri.substr(6), host, port, &parseError)) return fail("invalid tcp uri: " + parseError);
        return openTcpLink(host, port, kDefaultTcpConnectTimeout, error);
    }
    if (uri.starts_with("memory://")) {
        auto link = takeMemorySide(std::string(uri.substr(9)), true);
        if (!link) return fail("memory link already opened");
        return link;
    }
    if (uri.starts_with("serial://")) {
        std::uint32_t baud = 57600;
        if (auto b = param("baud")) {
            const auto parsed = parseNumber(*b);
            if (!parsed) return fail("invalid baud");
            baud = *parsed;
        }
        return openSerialLink(std::string(uri.substr(9)), baud, error);
    }
    return fail("unsupported link uri (use udp://, tcp://, serial:// or memory://)");
}

} // namespace gygax::net
