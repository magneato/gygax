#include <gygax/transport/modem.hpp>

#include <algorithm>
#include <charconv>
#include <string_view>
#include <thread>

namespace gygax::transport {

namespace {

constexpr std::string_view kConnectResponseToken = "CONNECT";
constexpr std::size_t kModemReadBufferBytes = 512;
constexpr std::chrono::milliseconds kModemReadPollInterval{100};
constexpr std::chrono::milliseconds kModemDataPollInterval{50};
constexpr std::chrono::milliseconds kModemEscapeSequenceResponseTimeout{1500};
constexpr const char* kModemTerminalConfigurationCommand = "ATE0V1Q0";

}

const char* toString(ModemCode code) {
    switch (code) {
    case ModemCode::Ok: return "OK";
    case ModemCode::Error: return "ERROR";
    case ModemCode::Connect: return "CONNECT";
    case ModemCode::NoCarrier: return "NO CARRIER";
    case ModemCode::Busy: return "BUSY";
    case ModemCode::NoDialtone: return "NO DIALTONE";
    case ModemCode::NoAnswer: return "NO ANSWER";
    case ModemCode::Ring: return "RING";
    case ModemCode::Timeout: return "TIMEOUT";
    }
    return "TIMEOUT";
}

ModemResponse HayesModem::parse(const std::string& text) {
    ModemResponse out;
    out.text = text;
    struct Rule {
        const char* token;
        ModemCode code;
    };
    static constexpr Rule rules[] = {
        {"NO CARRIER", ModemCode::NoCarrier},
        {"NO DIALTONE", ModemCode::NoDialtone},
        {"NO ANSWER", ModemCode::NoAnswer},
        {"BUSY", ModemCode::Busy},
        {"ERROR", ModemCode::Error},
        {kConnectResponseToken.data(), ModemCode::Connect},
        {"OK", ModemCode::Ok},
        {"RING", ModemCode::Ring},
    };
    std::size_t best = std::string::npos;
    for (const auto& r : rules) {
        const auto at = text.find(r.token);
        if (at != std::string::npos && at < best) {
            best = at;
            out.code = r.code;
        }
    }
    if (best == std::string::npos) {
        out.code = ModemCode::Timeout;
        return out;
    }
    if (out.code == ModemCode::Connect) {
        const auto at = text.find(kConnectResponseToken) + kConnectResponseToken.size();
        std::size_t i = at;
        while (i < text.size() && text[i] == ' ') ++i;
        int rate = 0;
        auto [ptr, ec] = std::from_chars(text.data() + i, text.data() + text.size(), rate);
        if (ec == std::errc() && ptr != text.data() + i) out.connectRate = rate;
    }
    return out;
}

ModemResponse HayesModem::await(std::chrono::milliseconds timeout, bool connectIsTerminal) {
    std::string collected;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (serial_.waitReadable(handle_, 1, std::min(remaining, kModemReadPollInterval)) != 0) continue;
        std::uint8_t buf[kModemReadBufferBytes];
        std::size_t n = 0;
        if (serial_.read(handle_, buf, sizeof(buf), n) != 0 || n == 0) continue;
        collected.append(reinterpret_cast<const char*>(buf), n);
        const auto tail = collected.find_last_of('\n');
        if (tail == std::string::npos) continue;
        auto parsed = parse(collected);
        if (parsed.code == ModemCode::Timeout) continue;
        if (parsed.code == ModemCode::Connect && !connectIsTerminal) continue;
        if (parsed.code == ModemCode::Ring) continue;
        return parsed;
    }
    ModemResponse timeoutResponse;
    timeoutResponse.text = collected;
    return timeoutResponse;
}

ModemResponse HayesModem::command(const std::string& command, std::chrono::milliseconds timeout) {
    (void)serial_.flushInput(handle_);
    if (serial_.writeAll(handle_, command + "\r") != 0) {
        ModemResponse r;
        r.code = ModemCode::Error;
        r.text = "write failed";
        return r;
    }
    return await(timeout, true);
}

ModemResponse HayesModem::initialize(const std::string& initString) {
    auto reset = command(initString);
    if (reset.code != ModemCode::Ok) return reset;
    return command(kModemTerminalConfigurationCommand);
}

ModemResponse HayesModem::dial(const std::string& number, bool pulse, std::chrono::milliseconds timeout) {
    if (number.empty() || number.find_first_not_of("0123456789,*#WP;T-") != std::string::npos) {
        ModemResponse r;
        r.code = ModemCode::Error;
        r.text = "invalid dial string";
        return r;
    }
    auto r = command(std::string(pulse ? "ATDP" : "ATDT") + number, timeout);
    online_ = r.code == ModemCode::Connect;
    return r;
}

ModemResponse HayesModem::answer(std::chrono::milliseconds timeout) {
    auto r = command("ATA", timeout);
    online_ = r.code == ModemCode::Connect;
    return r;
}

ModemResponse HayesModem::hangup(std::chrono::milliseconds guard) {
    if (online_) {
        std::this_thread::sleep_for(guard);
        (void)serial_.writeAll(handle_, "+++");
        std::this_thread::sleep_for(guard);
        (void)await(kModemEscapeSequenceResponseTimeout, true);
    }
    auto r = command("ATH0");
    if (r.code == ModemCode::Ok || r.code == ModemCode::NoCarrier) online_ = false;
    return r;
}

IOResult HayesModem::sendData(const std::string& data) {
    if (!online_) return -ENOTCONN;
    return serial_.writeAll(handle_, data);
}

std::string HayesModem::receiveData(std::chrono::milliseconds timeout) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)serial_.waitReadable(handle_, 1, kModemDataPollInterval);
        std::uint8_t buf[kModemReadBufferBytes];
        std::size_t n = 0;
        if (serial_.read(handle_, buf, sizeof(buf), n) == 0 && n > 0)
            out.append(reinterpret_cast<const char*>(buf), n);
        else if (!out.empty())
            break;
    }
    if (out.find("NO CARRIER") != std::string::npos) online_ = false;
    return out;
}

}
