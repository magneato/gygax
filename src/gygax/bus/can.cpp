#include <gygax/bus/can.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>

#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/time.h>
#define GYGAX_HAVE_SOCKETCAN 1
#endif

namespace gygax::bus {

namespace {

constexpr std::array<std::uint8_t, kMaxCanDlc + 1> kDlcLengths = {
    0, 1, 2, 3, 4, 5, 6, 7, static_cast<std::uint8_t>(kClassicCanPayloadBytes), 12, 16, 20, 24, 32, 48,
    static_cast<std::uint8_t>(kMaxCanFdPayloadBytes)};

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

std::uint8_t dlcToLength(std::uint8_t dlc) {
    return kDlcLengths[std::min<std::size_t>(dlc, kMaxCanDlc)];
}

std::uint8_t lengthToDlc(std::size_t length) {
    for (std::uint8_t dlc = 0; dlc <= kMaxCanDlc; ++dlc) {
        if (kDlcLengths[dlc] >= length) return dlc;
    }
    return kMaxCanDlc;
}

CanFrame CanFrame::make(std::uint32_t id, std::span<const std::uint8_t> bytes, bool extended, bool fd) {
    CanFrame f;
    f.id = id;
    f.extended = extended;
    f.fd = fd || bytes.size() > kClassicCanPayloadBytes;
    f.length = static_cast<std::uint8_t>(
        std::min<std::size_t>(bytes.size(), f.fd ? kMaxCanFdPayloadBytes : kClassicCanPayloadBytes));
    std::copy_n(bytes.begin(), f.length, f.data.begin());
    return f;
}

bool operator==(const CanFrame& a, const CanFrame& b) {
    return a.id == b.id && a.extended == b.extended && a.remote == b.remote && a.fd == b.fd && a.length == b.length &&
           std::equal(a.data.begin(), a.data.begin() + a.length, b.data.begin());
}

std::string toString(const CanFrame& frame) {
    std::string out = frame.extended ? std::format("{:08X}", frame.id) : std::format("{:03X}", frame.id);
    out += frame.fd ? "##" : "#";
    if (frame.remote) return out + "R";
    for (std::size_t i = 0; i < frame.length; ++i) out += std::format("{:02X}", frame.data[i]);
    return out;
}

class LoopbackCanNetwork::Node final : public CanBus {
public:
    Node(LoopbackCanNetwork& network, std::string name, bool own) : network_(network), name_(std::move(name)), own_(own) {}

    [[nodiscard]] IOResult send(const CanFrame& frame) override {
        if (frame.length > (frame.fd ? kMaxCanFdPayloadBytes : kClassicCanPayloadBytes)) return -EMSGSIZE;
        if (frame.id > frame.maxId()) return -EINVAL;
        CanFrame stamped = frame;
        stamped.timestampNs = nowNs();
        network_.broadcast(this, stamped);
        return 0;
    }

    [[nodiscard]] IOResult receive(CanFrame& frame, std::chrono::milliseconds timeout) override {
        std::unique_lock lock(mutex_);
        if (!ready_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) return -ETIMEDOUT;
        frame = queue_.front();
        queue_.pop_front();
        return 0;
    }

    [[nodiscard]] IOResult setFilters(const std::vector<CanFilter>& filters) override {
        std::lock_guard lock(mutex_);
        filters_ = filters;
        return 0;
    }

    [[nodiscard]] std::string name() const override { return name_; }

    void deliver(const Node* from, const CanFrame& frame) {
        std::lock_guard lock(mutex_);
        if (from == this && !own_) return;
        if (!filters_.empty() && std::none_of(filters_.begin(), filters_.end(), [&](const CanFilter& f) { return f.matches(frame); }))
            return;
        if (queue_.size() >= kQueueLimit) queue_.pop_front();
        queue_.push_back(frame);
        ready_.notify_one();
    }

private:
    static constexpr std::size_t kQueueLimit = 4096;

    LoopbackCanNetwork& network_;
    std::string name_;
    bool own_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<CanFrame> queue_;
    std::vector<CanFilter> filters_;
};

std::shared_ptr<CanBus> LoopbackCanNetwork::attach(std::string name, bool receiveOwnFrames) {
    auto node = std::make_shared<Node>(*this, std::move(name), receiveOwnFrames);
    std::lock_guard lock(mutex_);
    std::erase_if(nodes_, [](const std::weak_ptr<Node>& w) { return w.expired(); });
    nodes_.push_back(node);
    return node;
}

void LoopbackCanNetwork::broadcast(const Node* from, const CanFrame& frame) {
    std::vector<std::shared_ptr<Node>> targets;
    {
        std::lock_guard lock(mutex_);
        ++carried_;
        for (const auto& w : nodes_) {
            if (auto n = w.lock()) targets.push_back(std::move(n));
        }
    }
    for (const auto& n : targets) n->deliver(from, frame);
}

std::uint64_t LoopbackCanNetwork::framesCarried() const {
    std::lock_guard lock(mutex_);
    return carried_;
}

#if defined(GYGAX_HAVE_SOCKETCAN)

bool SocketCanBus::supported() {
    return true;
}

std::unique_ptr<SocketCanBus> SocketCanBus::open(const std::string& interfaceName, bool canFd, std::string* error) {
    auto fail = [&](const std::string& message) -> std::unique_ptr<SocketCanBus> {
        if (error != nullptr) *error = message;
        return nullptr;
    };
    if (interfaceName.empty() || interfaceName.size() >= IFNAMSIZ) return fail("invalid CAN interface name");
    const int fd = ::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    if (fd < 0) return fail(std::string("cannot open a CAN socket: ") + std::strerror(errno));
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, interfaceName.c_str(), IFNAMSIZ - 1);
    if (::ioctl(fd, SIOCGIFINDEX, &ifr) != 0) {
        const int err = errno;
        ::close(fd);
        return fail(std::format("no such CAN interface '{}': {}", interfaceName, std::strerror(err)));
    }
    if (canFd) {
        const int enable = 1;
        if (::setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable)) != 0) {
            const int err = errno;
            ::close(fd);
            return fail(std::string("CAN FD is not available on this interface: ") + std::strerror(err));
        }
    }
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int err = errno;
        ::close(fd);
        return fail(std::string("cannot bind the CAN socket: ") + std::strerror(err));
    }
    return std::unique_ptr<SocketCanBus>(new SocketCanBus(fd, interfaceName, canFd));
}

SocketCanBus::~SocketCanBus() {
    if (fd_ >= 0) ::close(fd_);
}

IOResult SocketCanBus::send(const CanFrame& frame) {
    if (frame.id > frame.maxId()) return -EINVAL;
    const bool needsFd = frame.fd || frame.length > 8;
    if (needsFd && !fd_frames_) return -EPROTONOSUPPORT;
    canid_t id = frame.id;
    if (frame.extended) id |= CAN_EFF_FLAG;
    if (frame.remote) id |= CAN_RTR_FLAG;
    if (needsFd) {
        if (frame.length > kMaxCanFdPayloadBytes) return -EMSGSIZE;
        canfd_frame out{};
        out.can_id = id;
        out.len = frame.length;
        out.flags = frame.bitrateSwitch ? CANFD_BRS : 0;
        std::memcpy(out.data, frame.data.data(), frame.length);
        const ssize_t n = ::write(fd_, &out, sizeof(out));
        return n == static_cast<ssize_t>(sizeof(out)) ? 0 : -errno;
    }
    can_frame out{};
    out.can_id = id;
    out.len = frame.length;
    std::memcpy(out.data, frame.data.data(), frame.length);
    const ssize_t n = ::write(fd_, &out, sizeof(out));
    return n == static_cast<ssize_t>(sizeof(out)) ? 0 : -errno;
}

IOResult SocketCanBus::receive(CanFrame& frame, std::chrono::milliseconds timeout) {
    pollfd p{fd_, POLLIN, 0};
    const int rc = ::poll(&p, 1, static_cast<int>(timeout.count()));
    if (rc == 0) return -ETIMEDOUT;
    if (rc < 0) return -errno;
    canfd_frame raw{};
    const ssize_t n = ::read(fd_, &raw, sizeof(raw));
    if (n < 0) return -errno;
    if (n != static_cast<ssize_t>(CAN_MTU) && n != static_cast<ssize_t>(CANFD_MTU)) return -EPROTO;
    frame = CanFrame{};
    frame.extended = (raw.can_id & CAN_EFF_FLAG) != 0;
    frame.remote = (raw.can_id & CAN_RTR_FLAG) != 0;
    frame.id = raw.can_id & (frame.extended ? CAN_EFF_MASK : CAN_SFF_MASK);
    frame.fd = n == static_cast<ssize_t>(CANFD_MTU);
    frame.bitrateSwitch = frame.fd && (raw.flags & CANFD_BRS) != 0;
    frame.length = static_cast<std::uint8_t>(std::min<std::size_t>(raw.len, kMaxCanFdPayloadBytes));
    std::memcpy(frame.data.data(), raw.data, frame.length);
    timeval tv{};
    if (::ioctl(fd_, SIOCGSTAMP, &tv) == 0)
        frame.timestampNs = static_cast<std::uint64_t>(tv.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(tv.tv_usec) * 1000ULL;
    return 0;
}

IOResult SocketCanBus::setFilters(const std::vector<CanFilter>& filters) {
    std::vector<can_filter> raw;
    raw.reserve(filters.size());
    for (const auto& f : filters) {
        can_filter cf{};
        cf.can_id = f.id | (f.extended ? CAN_EFF_FLAG : 0);
        cf.can_mask = f.mask | CAN_EFF_FLAG;
        raw.push_back(cf);
    }
    return ::setsockopt(fd_, SOL_CAN_RAW, CAN_RAW_FILTER, raw.data(), static_cast<socklen_t>(raw.size() * sizeof(can_filter))) == 0
               ? 0
               : -errno;
}

#else

bool SocketCanBus::supported() {
    return false;
}

std::unique_ptr<SocketCanBus> SocketCanBus::open(const std::string&, bool, std::string* error) {
    if (error != nullptr) *error = "SocketCAN is only available on Linux";
    return nullptr;
}

SocketCanBus::~SocketCanBus() = default;
IOResult SocketCanBus::send(const CanFrame&) {
    return -ENOTSUP;
}
IOResult SocketCanBus::receive(CanFrame&, std::chrono::milliseconds) {
    return -ENOTSUP;
}
IOResult SocketCanBus::setFilters(const std::vector<CanFilter>&) {
    return -ENOTSUP;
}

#endif

} // namespace gygax::bus
