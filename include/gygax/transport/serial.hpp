#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <gygax/transport/base.hpp>

namespace gygax::transport {

inline constexpr std::chrono::milliseconds kDefaultSerialWriteTimeout{2000};

class SerialTransport final : public Transport {
public:
    SerialTransport();
    ~SerialTransport() override;
    SerialTransport(const SerialTransport&) = delete;
    SerialTransport& operator=(const SerialTransport&) = delete;

    [[nodiscard]] IOResult initialize() override;
    [[nodiscard]] IOResult shutdown() override;
    [[nodiscard]] std::vector<std::string> enumerateDevices() const override;
    [[nodiscard]] IOResult open(const std::string& devicePath, TransportHandle& handleOut) override;
    [[nodiscard]] IOResult close(TransportHandle handle) override;
    [[nodiscard]] IOResult write(TransportHandle handle, const std::uint8_t* data, std::size_t length, std::size_t& written) override;
    [[nodiscard]] IOResult read(TransportHandle handle, std::uint8_t* buffer, std::size_t capacity, std::size_t& count) override;
    [[nodiscard]] IOResult available(TransportHandle handle, std::size_t& count) const override;
    [[nodiscard]] IOResult setCallback(TransportHandle handle, TransferCallback callback) override;

    [[nodiscard]] IOResult configure(TransportHandle handle, BaudRate baud, DataBits dataBits = DataBits::Bits8,
                                     StopBits stopBits = StopBits::Bits1, Parity parity = Parity::None,
                                     FlowControl flow = FlowControl::None);
    [[nodiscard]] IOResult setLine(TransportHandle handle, const std::string& line, bool asserted);
    [[nodiscard]] IOResult getLine(TransportHandle handle, const std::string& line, bool& asserted) const;
    [[nodiscard]] IOResult flushInput(TransportHandle handle);
    [[nodiscard]] IOResult waitReadable(TransportHandle handle, std::size_t minBytes, std::chrono::milliseconds timeout);
    [[nodiscard]] IOResult writeAll(TransportHandle handle, const std::string& data,
                                    std::chrono::milliseconds timeout = kDefaultSerialWriteTimeout);

private:
    struct Port {
        int fd = -1;
        std::string rx;
        TransferCallback callback;
        std::uint64_t dropped = 0;
    };

    void readerLoop();
    void wake() const;

    static constexpr std::size_t kMaxRx = 1 << 20;

    mutable std::mutex mutex_;
    std::condition_variable readable_;
    std::map<TransportHandle, Port> ports_;
    TransportHandle nextHandle_ = 1;
    std::thread reader_;
    bool running_ = false;
    int wakeRead_ = -1;
    int wakeWrite_ = -1;
};

}
