#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include <gygax/net/link.hpp>

namespace gygax::bus::modbus {

using net::IOResult;

inline constexpr std::uint8_t kDefaultUnitId = 1;
inline constexpr std::uint8_t kReadHoldingRegistersFunctionCode = 3;
inline constexpr std::chrono::milliseconds kDefaultClientTimeout{1000};

enum class Mode { Rtu, Tcp };

std::uint16_t crc16(std::span<const std::uint8_t> data);

struct Request {
    std::uint8_t unit = kDefaultUnitId;
    std::uint8_t function = kReadHoldingRegistersFunctionCode;
    std::vector<std::uint8_t> data;
};

std::vector<std::uint8_t> frameRtu(std::uint8_t unit, std::uint8_t function, std::span<const std::uint8_t> data);
std::vector<std::uint8_t> frameTcp(std::uint16_t transaction, std::uint8_t unit, std::uint8_t function, std::span<const std::uint8_t> data);
std::optional<Request> parseRtu(std::span<const std::uint8_t> frame);
std::optional<Request> parseTcp(std::span<const std::uint8_t> frame, std::uint16_t* transaction = nullptr);
const char* exceptionText(std::uint8_t code);

class Client {
public:
    Client(std::shared_ptr<net::ByteLink> link, Mode mode, std::chrono::milliseconds timeout = kDefaultClientTimeout);

    [[nodiscard]] IOResult readCoils(std::uint8_t unit, std::uint16_t address, std::uint16_t count, std::vector<bool>& out);
    [[nodiscard]] IOResult readDiscreteInputs(std::uint8_t unit, std::uint16_t address, std::uint16_t count, std::vector<bool>& out);
    [[nodiscard]] IOResult readHoldingRegisters(std::uint8_t unit, std::uint16_t address, std::uint16_t count,
                                                std::vector<std::uint16_t>& out);
    [[nodiscard]] IOResult readInputRegisters(std::uint8_t unit, std::uint16_t address, std::uint16_t count,
                                              std::vector<std::uint16_t>& out);
    [[nodiscard]] IOResult writeCoil(std::uint8_t unit, std::uint16_t address, bool value);
    [[nodiscard]] IOResult writeRegister(std::uint8_t unit, std::uint16_t address, std::uint16_t value);
    [[nodiscard]] IOResult writeCoils(std::uint8_t unit, std::uint16_t address, const std::vector<bool>& values);
    [[nodiscard]] IOResult writeRegisters(std::uint8_t unit, std::uint16_t address, const std::vector<std::uint16_t>& values);
    [[nodiscard]] IOResult maskWriteRegister(std::uint8_t unit, std::uint16_t address, std::uint16_t andMask, std::uint16_t orMask);
    [[nodiscard]] IOResult call(std::uint8_t unit, std::uint8_t function, std::span<const std::uint8_t> data,
                                std::vector<std::uint8_t>& response);

    [[nodiscard]] std::uint8_t lastException() const { return exception_; }

    static float registersToFloat(std::uint16_t high, std::uint16_t low);
    static std::pair<std::uint16_t, std::uint16_t> floatToRegisters(float value);

private:
    [[nodiscard]] IOResult readBits(std::uint8_t function, std::uint8_t unit, std::uint16_t address, std::uint16_t count,
                                    std::vector<bool>& out);
    [[nodiscard]] IOResult readWords(std::uint8_t function, std::uint8_t unit, std::uint16_t address, std::uint16_t count,
                                     std::vector<std::uint16_t>& out);

    std::shared_ptr<net::ByteLink> link_;
    Mode mode_;
    std::chrono::milliseconds timeout_;
    std::uint16_t transaction_ = 0;
    std::uint8_t exception_ = 0;
    std::vector<std::uint8_t> pending_;
};

class Slave {
public:
    explicit Slave(std::uint8_t unit) : unit_(unit) {}

    std::map<std::uint16_t, std::uint16_t> holding;
    std::map<std::uint16_t, std::uint16_t> input;
    std::map<std::uint16_t, bool> coils;
    std::map<std::uint16_t, bool> discrete;

    std::optional<std::vector<std::uint8_t>> process(const Request& request);
    bool serveOne(net::ByteLink& link, Mode mode, std::chrono::milliseconds timeout);

private:
    std::uint8_t unit_;
    std::vector<std::uint8_t> buffer_;
};

} // namespace gygax::bus::modbus
