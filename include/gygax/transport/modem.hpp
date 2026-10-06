#pragma once

#include <chrono>
#include <string>

#include <gygax/transport/serial.hpp>

namespace gygax::transport {

inline constexpr std::chrono::milliseconds kDefaultModemCommandTimeout{2000};
inline constexpr std::chrono::milliseconds kDefaultModemDialTimeout{60000};
inline constexpr std::chrono::milliseconds kDefaultModemAnswerTimeout{60000};
inline constexpr std::chrono::milliseconds kDefaultModemEscapeGuard{1100};
inline constexpr const char* kDefaultModemResetCommand = "ATZ";

enum class ModemCode { Ok, Error, Connect, NoCarrier, Busy, NoDialtone, NoAnswer, Ring, Timeout };

struct ModemResponse {
    ModemCode code = ModemCode::Timeout;
    int connectRate = 0;
    std::string text;
};

class HayesModem {
public:
    HayesModem(SerialTransport& serial, TransportHandle handle) : serial_(serial), handle_(handle) {}

    ModemResponse command(const std::string& command, std::chrono::milliseconds timeout = kDefaultModemCommandTimeout);
    ModemResponse initialize(const std::string& initString = kDefaultModemResetCommand);
    ModemResponse dial(const std::string& number, bool pulse = false, std::chrono::milliseconds timeout = kDefaultModemDialTimeout);
    ModemResponse answer(std::chrono::milliseconds timeout = kDefaultModemAnswerTimeout);
    ModemResponse hangup(std::chrono::milliseconds guard = kDefaultModemEscapeGuard);
    IOResult sendData(const std::string& data);
    std::string receiveData(std::chrono::milliseconds timeout);
    [[nodiscard]] bool online() const { return online_; }

    static ModemResponse parse(const std::string& text);

private:
    ModemResponse await(std::chrono::milliseconds timeout, bool connectIsTerminal);

    SerialTransport& serial_;
    TransportHandle handle_;
    bool online_ = false;
};

const char* toString(ModemCode code);

}
