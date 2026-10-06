#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <gygax/bus/can.hpp>

namespace gygax::bus {

struct IsoTpOptions {
    std::uint32_t txId = 0x7E0;
    std::uint32_t rxId = 0x7E8;
    bool extended = false;
    bool fd = false;
    bool padding = true;
    std::uint8_t paddingByte = 0xCC;
    std::uint8_t blockSize = 0;
    std::chrono::microseconds separationTime{0};
    std::chrono::milliseconds timeout{1000};
    std::uint8_t maxWaitFrames = 8;
};

class IsoTpChannel {
public:
    IsoTpChannel(std::shared_ptr<CanBus> bus, IsoTpOptions options) : bus_(std::move(bus)), options_(options) {}

    [[nodiscard]] IOResult send(std::span<const std::uint8_t> payload);
    [[nodiscard]] IOResult receive(std::vector<std::uint8_t>& payload, std::chrono::milliseconds timeout);
    [[nodiscard]] const IsoTpOptions& options() const { return options_; }

    static constexpr std::size_t kMaxPayload = 4095;

private:
    [[nodiscard]] IOResult sendFrame(std::span<const std::uint8_t> bytes);
    [[nodiscard]] IOResult sendFlowControl(std::uint8_t status);
    [[nodiscard]] IOResult awaitFlowControl(std::uint8_t& blockSize, std::chrono::microseconds& stMin);
    [[nodiscard]] bool nextFrame(CanFrame& frame, std::chrono::steady_clock::time_point deadline);

    std::shared_ptr<CanBus> bus_;
    IsoTpOptions options_;
};

} // namespace gygax::bus
