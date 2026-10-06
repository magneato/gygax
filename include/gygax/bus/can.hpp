#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <gygax/transport/base.hpp>

namespace gygax::bus {

using transport::IOResult;

inline constexpr std::size_t kMaxCanFdPayloadBytes = 64;
inline constexpr std::size_t kClassicCanPayloadBytes = 8;
inline constexpr std::uint8_t kMaxCanDlc = 15;
inline constexpr std::uint32_t kStandardCanMaxIdentifier = 0x7FFU;
inline constexpr std::uint32_t kExtendedCanMaxIdentifier = 0x1FFFFFFFU;

struct CanFrame {
    std::uint32_t id = 0;
    bool extended = false;
    bool remote = false;
    bool fd = false;
    bool bitrateSwitch = false;
    std::uint8_t length = 0;
    std::array<std::uint8_t, kMaxCanFdPayloadBytes> data{};
    std::uint64_t timestampNs = 0;

    [[nodiscard]] std::span<const std::uint8_t> payload() const { return {data.data(), length}; }
    [[nodiscard]] std::uint32_t maxId() const {
        return extended ? kExtendedCanMaxIdentifier : kStandardCanMaxIdentifier;
    }

    static CanFrame make(std::uint32_t id, std::span<const std::uint8_t> bytes, bool extended = false, bool fd = false);
    friend bool operator==(const CanFrame& a, const CanFrame& b);
};

struct CanFilter {
    std::uint32_t id = 0;
    std::uint32_t mask = 0;
    bool extended = false;
    [[nodiscard]] bool matches(const CanFrame& f) const { return f.extended == extended && (f.id & mask) == (id & mask); }
};

std::uint8_t dlcToLength(std::uint8_t dlc);
std::uint8_t lengthToDlc(std::size_t length);
std::string toString(const CanFrame& frame);

class CanBus {
public:
    virtual ~CanBus() = default;
    [[nodiscard]] virtual IOResult send(const CanFrame& frame) = 0;
    [[nodiscard]] virtual IOResult receive(CanFrame& frame, std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual IOResult setFilters(const std::vector<CanFilter>& filters) = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};

class LoopbackCanNetwork {
public:
    LoopbackCanNetwork() = default;
    LoopbackCanNetwork(const LoopbackCanNetwork&) = delete;
    LoopbackCanNetwork& operator=(const LoopbackCanNetwork&) = delete;

    std::shared_ptr<CanBus> attach(std::string name, bool receiveOwnFrames = false);
    [[nodiscard]] std::uint64_t framesCarried() const;

private:
    class Node;
    void broadcast(const Node* from, const CanFrame& frame);

    mutable std::mutex mutex_;
    std::vector<std::weak_ptr<Node>> nodes_;
    std::uint64_t carried_ = 0;
};

class SocketCanBus final : public CanBus {
public:
    static std::unique_ptr<SocketCanBus> open(const std::string& interfaceName, bool canFd, std::string* error = nullptr);
    ~SocketCanBus() override;
    SocketCanBus(const SocketCanBus&) = delete;
    SocketCanBus& operator=(const SocketCanBus&) = delete;

    [[nodiscard]] IOResult send(const CanFrame& frame) override;
    [[nodiscard]] IOResult receive(CanFrame& frame, std::chrono::milliseconds timeout) override;
    [[nodiscard]] IOResult setFilters(const std::vector<CanFilter>& filters) override;
    [[nodiscard]] std::string name() const override { return interface_; }
    [[nodiscard]] static bool supported();

private:
    SocketCanBus(int fd, std::string interfaceName, bool canFd) : fd_(fd), interface_(std::move(interfaceName)), fd_frames_(canFd) {}

    int fd_;
    std::string interface_;
    bool fd_frames_;
};

} // namespace gygax::bus
