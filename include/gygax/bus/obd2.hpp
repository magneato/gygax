#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gygax/bus/isotp.hpp>

namespace gygax::bus {

inline constexpr std::chrono::milliseconds kDefaultObd2Timeout{500};

struct Obd2Value {
    std::uint8_t pid = 0;
    std::string name;
    double value = 0.0;
    std::string unit;
};

class Obd2Client {
public:
    explicit Obd2Client(std::shared_ptr<CanBus> bus, std::uint8_t ecuIndex = 0,
                        std::chrono::milliseconds timeout = kDefaultObd2Timeout);

    [[nodiscard]] IOResult request(std::uint8_t mode, std::span<const std::uint8_t> arguments, std::vector<std::uint8_t>& response);
    [[nodiscard]] std::optional<Obd2Value> readPid(std::uint8_t pid);
    [[nodiscard]] std::vector<std::uint8_t> supportedPids();
    [[nodiscard]] std::vector<std::string> readStoredDtcs();
    [[nodiscard]] std::string readVin();

    static std::optional<Obd2Value> decode(std::uint8_t pid, std::span<const std::uint8_t> data);
    static std::string formatDtc(std::uint8_t high, std::uint8_t low);
    static const char* pidName(std::uint8_t pid);

private:
    IsoTpChannel channel_;
    std::chrono::milliseconds timeout_;
};

class VirtualObdEcu {
public:
    using PidSource = std::function<std::optional<double>(std::uint8_t pid)>;

    VirtualObdEcu(std::shared_ptr<CanBus> bus, std::uint8_t ecuIndex, PidSource source);
    ~VirtualObdEcu();
    VirtualObdEcu(const VirtualObdEcu&) = delete;
    VirtualObdEcu& operator=(const VirtualObdEcu&) = delete;

    void setVin(std::string vin) { vin_ = std::move(vin); }
    void setDtcs(std::vector<std::uint16_t> codes) { dtcs_ = std::move(codes); }
    void start();
    void stop();
    bool serveOne(std::chrono::milliseconds timeout);
    [[nodiscard]] std::uint64_t requestsServed() const { return served_.load(); }

    static std::optional<std::vector<std::uint8_t>> encode(std::uint8_t pid, double value);

private:
    IsoTpChannel channel_;
    PidSource source_;
    std::string vin_ = "1GYGAX00000000000";
    std::vector<std::uint16_t> dtcs_;
    std::atomic<std::uint64_t> served_{0};
    std::jthread worker_;
};

} // namespace gygax::bus
