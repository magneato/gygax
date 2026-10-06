#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gygax::transport {

using TransportHandle = std::uint64_t;
using IOResult = std::int32_t;

enum class BaudRate : std::uint32_t {
    Baud300 = 300,
    Baud1200 = 1200,
    Baud2400 = 2400,
    Baud4800 = 4800,
    Baud9600 = 9600,
    Baud19200 = 19200,
    Baud38400 = 38400,
    Baud57600 = 57600,
    Baud115200 = 115200,
    Baud230400 = 230400,
};

enum class DataBits : std::uint8_t { Bits5 = 5, Bits6 = 6, Bits7 = 7, Bits8 = 8 };
enum class StopBits : std::uint8_t { Bits1 = 1, Bits2 = 2 };
enum class Parity : std::uint8_t { None = 0, Even = 1, Odd = 2 };
enum class FlowControl : std::uint8_t { None = 0, RtsCts = 1, XonXoff = 2 };
enum class GpioMode : std::uint8_t { Input = 0, Output = 1 };
enum class GpioLevel : std::uint8_t { Low = 0, High = 1 };

using TransferCallback = std::function<void(TransportHandle handle, const std::uint8_t* data, std::size_t length)>;
using GpioCallback = std::function<void(std::uint32_t pin, GpioLevel level, std::uint64_t timestampNs)>;

class Transport {
public:
    virtual ~Transport() = default;
    [[nodiscard]] virtual IOResult initialize() = 0;
    [[nodiscard]] virtual IOResult shutdown() = 0;
    [[nodiscard]] virtual std::vector<std::string> enumerateDevices() const = 0;
    [[nodiscard]] virtual IOResult open(const std::string& devicePath, TransportHandle& handleOut) = 0;
    [[nodiscard]] virtual IOResult close(TransportHandle handle) = 0;
    [[nodiscard]] virtual IOResult write(TransportHandle handle, const std::uint8_t* data, std::size_t length, std::size_t& written) = 0;
    [[nodiscard]] virtual IOResult read(TransportHandle handle, std::uint8_t* buffer, std::size_t capacity, std::size_t& count) = 0;
    [[nodiscard]] virtual IOResult available(TransportHandle handle, std::size_t& count) const = 0;
    [[nodiscard]] virtual IOResult setCallback(TransportHandle handle, TransferCallback callback) = 0;
};

class GpioPort {
public:
    virtual ~GpioPort() = default;
    [[nodiscard]] virtual IOResult initialize() = 0;
    [[nodiscard]] virtual IOResult shutdown() = 0;
    [[nodiscard]] virtual IOResult configure(std::uint32_t pin, GpioMode mode) = 0;
    [[nodiscard]] virtual IOResult read(std::uint32_t pin, GpioLevel& level) const = 0;
    [[nodiscard]] virtual IOResult write(std::uint32_t pin, GpioLevel level) = 0;
    [[nodiscard]] virtual IOResult watch(std::uint32_t pin, GpioCallback callback) = 0;
    [[nodiscard]] virtual IOResult unwatch(std::uint32_t pin) = 0;
};

std::string describeError(IOResult result);

}
