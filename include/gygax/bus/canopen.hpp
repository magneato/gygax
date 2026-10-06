#pragma once

#include <chrono>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <gygax/bus/can.hpp>

namespace gygax::bus::canopen {

enum class NmtCommand : std::uint8_t { Start = 0x01, Stop = 0x02, PreOperational = 0x80, ResetNode = 0x81, ResetCommunication = 0x82 };
enum class NmtState : std::uint8_t { BootUp = 0x00, Stopped = 0x04, Operational = 0x05, PreOperational = 0x7F };

struct Heartbeat {
    std::uint8_t nodeId = 0;
    NmtState state = NmtState::BootUp;
};

CanFrame nmtFrame(NmtCommand command, std::uint8_t nodeId);
CanFrame syncFrame();
std::optional<Heartbeat> parseHeartbeat(const CanFrame& frame);
const char* abortText(std::uint32_t code);

class SdoClient {
public:
    explicit SdoClient(std::shared_ptr<CanBus> bus, std::chrono::milliseconds timeout = std::chrono::milliseconds(500))
        : bus_(std::move(bus)), timeout_(timeout) {}

    [[nodiscard]] IOResult read(std::uint8_t nodeId, std::uint16_t index, std::uint8_t subIndex, std::vector<std::uint8_t>& out);
    [[nodiscard]] IOResult write(std::uint8_t nodeId, std::uint16_t index, std::uint8_t subIndex, std::span<const std::uint8_t> data);
    [[nodiscard]] std::uint32_t lastAbortCode() const { return abort_; }

private:
    [[nodiscard]] IOResult exchange(std::uint8_t nodeId, const std::uint8_t (&request)[8], CanFrame& reply);
    [[nodiscard]] IOResult awaitReply(std::uint8_t nodeId, CanFrame& reply);

    std::shared_ptr<CanBus> bus_;
    std::chrono::milliseconds timeout_;
    std::uint32_t abort_ = 0;
};

class SdoServer {
public:
    using Reader = std::function<std::optional<std::vector<std::uint8_t>>(std::uint16_t index, std::uint8_t sub)>;
    using Writer = std::function<bool(std::uint16_t index, std::uint8_t sub, std::span<const std::uint8_t> data)>;

    SdoServer(std::shared_ptr<CanBus> bus, std::uint8_t nodeId, Reader reader, Writer writer)
        : bus_(std::move(bus)), nodeId_(nodeId), reader_(std::move(reader)), writer_(std::move(writer)) {}

    bool serveOne(std::chrono::milliseconds timeout);

private:
    void abort(std::uint16_t index, std::uint8_t sub, std::uint32_t code);

    std::shared_ptr<CanBus> bus_;
    std::uint8_t nodeId_;
    Reader reader_;
    Writer writer_;
    std::vector<std::uint8_t> pendingUpload_;
    std::size_t uploadOffset_ = 0;
    bool uploadToggle_ = false;
    std::vector<std::uint8_t> pendingDownload_;
    std::size_t downloadSize_ = 0;
    std::uint16_t downloadIndex_ = 0;
    std::uint8_t downloadSub_ = 0;
    bool downloadToggle_ = false;
    bool downloading_ = false;
};

} // namespace gygax::bus::canopen
