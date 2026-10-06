#include <gygax/transport/gcode.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>

#include <gygax/core/charconv.hpp>
#include <cstdio>
#include <format>

namespace gygax::transport {

namespace {

constexpr std::size_t kSerialReadBufferSize = 512;
constexpr double kMinimumTargetTemperatureCelsius = 0.0;
constexpr double kMaximumHotendTemperatureCelsius = 400.0;
constexpr double kMaximumBedTemperatureCelsius = 150.0;

std::optional<double> numberAfter(const std::string& text, const std::string& key, std::size_t from = 0) {
    const auto at = text.find(key, from);
    if (at == std::string::npos) return std::nullopt;
    const char* first = text.data() + at + key.size();
    const char* last = text.data() + text.size();
    double v = 0.0;
    auto [ptr, ec] = gygax::fromChars(first, last, v);
    if (ec != std::errc() || ptr == first) return std::nullopt;
    return v;
}

}

GcodePrinter::GcodePrinter(SerialTransport& serial, TransportHandle handle, GcodeOptions options)
    : serial_(serial), handle_(handle), options_(options) {}

int GcodePrinter::checksum(const std::string& text) {
    int cs = 0;
    for (const char c : text) cs ^= static_cast<unsigned char>(c);
    return cs;
}

std::string GcodePrinter::stripComment(const std::string& line) {
    std::string out = line.substr(0, line.find(';'));
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())) != 0) out.pop_back();
    std::size_t start = 0;
    while (start < out.size() && std::isspace(static_cast<unsigned char>(out[start])) != 0) ++start;
    return out.substr(start);
}

std::optional<Temperatures> GcodePrinter::parseTemperatures(const std::string& text) {
    const auto at = text.find("T:");
    if (at == std::string::npos) return std::nullopt;
    Temperatures t;
    const auto hot = numberAfter(text, "T:", at);
    if (!hot) return std::nullopt;
    t.hotend = *hot;
    const auto slash = text.find('/', at);
    if (slash != std::string::npos) t.hotendTarget = numberAfter(text, "/", at).value_or(0.0);
    if (const auto bed = text.find("B:", at); bed != std::string::npos) {
        t.bed = numberAfter(text, "B:", bed).value_or(0.0);
        const auto bslash = text.find('/', bed);
        if (bslash != std::string::npos) t.bedTarget = numberAfter(text, "/", bed).value_or(0.0);
    }
    return t;
}

std::optional<Position> GcodePrinter::parsePosition(const std::string& text) {
    const auto x = numberAfter(text, "X:");
    const auto y = numberAfter(text, "Y:");
    const auto z = numberAfter(text, "Z:");
    if (!x || !y || !z) return std::nullopt;
    Position p;
    p.x = *x;
    p.y = *y;
    p.z = *z;
    p.e = numberAfter(text, "E:").value_or(0.0);
    return p;
}

IOResult GcodePrinter::connect(std::chrono::milliseconds banner) {
    (void)serial_.waitReadable(handle_, 1, banner);
    (void)serial_.flushInput(handle_);
    nextLine_ = 1;
    std::string reply;
    const bool saved = options_.lineNumbers;
    options_.lineNumbers = false;
    const auto rc = send("M110 N0", &reply);
    options_.lineNumbers = saved;
    return rc;
}

IOResult GcodePrinter::send(const std::string& command, std::string* response) {
    const std::string clean = stripComment(command);
    if (clean.empty()) return 0;
    if (clean.find_first_of("\r\n") != std::string::npos) return -EINVAL;

    for (int attempt = 0; attempt <= options_.maxResends; ++attempt) {
        std::string wire = clean;
        const int lineNo = nextLine_;
        if (options_.lineNumbers) {
            wire = std::format("N{} {}", lineNo, clean);
            wire += std::format("*{}", checksum(wire));
        }
        (void)serial_.flushInput(handle_);
        if (const auto rc = serial_.writeAll(handle_, wire + "\n"); rc != 0) return rc;

        std::string collected;
        const auto deadline = std::chrono::steady_clock::now() + options_.commandTimeout;
        bool resend = false;
        bool acknowledged = false;
        while (!acknowledged && !resend && std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (serial_.waitReadable(handle_, 1, std::min(remaining, std::chrono::milliseconds(100))) != 0) continue;
            std::uint8_t buf[kSerialReadBufferSize];
            std::size_t n = 0;
            if (serial_.read(handle_, buf, sizeof(buf), n) != 0) continue;
            collected.append(reinterpret_cast<const char*>(buf), n);
            std::size_t pos = 0;
            while (true) {
                const auto nl = collected.find('\n', pos);
                if (nl == std::string::npos) break;
                const auto line = collected.substr(pos, nl - pos);
                pos = nl + 1;
                if (line.starts_with("Resend:")) resend = true;
                if (line.starts_with("ok")) acknowledged = true;
                if (line.starts_with("Error:") && line.find("checksum") == std::string::npos &&
                    line.find("Line Number") == std::string::npos) {
                    if (response != nullptr) *response = collected;
                    return -EIO;
                }
            }
        }
        if (acknowledged && !resend) {
            if (options_.lineNumbers) ++nextLine_;
            if (response != nullptr) *response = collected;
            return 0;
        }
        if (!resend) return -ETIMEDOUT;
    }
    return -EIO;
}

IOResult GcodePrinter::stream(std::istream& program, const Progress& progress) {
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(program, line)) {
        auto clean = stripComment(line);
        if (!clean.empty()) lines.push_back(std::move(clean));
    }
    std::size_t sent = 0;
    for (const auto& cmd : lines) {
        if (const auto rc = send(cmd); rc != 0) return rc;
        ++sent;
        if (progress) progress(sent, lines.size());
    }
    return 0;
}

IOResult GcodePrinter::readTemperatures(Temperatures& out) {
    std::string reply;
    if (const auto rc = send("M105", &reply); rc != 0) return rc;
    auto parsed = parseTemperatures(reply);
    if (!parsed) return -EPROTO;
    out = *parsed;
    return 0;
}

IOResult GcodePrinter::readPosition(Position& out) {
    std::string reply;
    if (const auto rc = send("M114", &reply); rc != 0) return rc;
    auto parsed = parsePosition(reply);
    if (!parsed) return -EPROTO;
    out = *parsed;
    return 0;
}

IOResult GcodePrinter::setHotend(double celsius) {
    if (celsius < kMinimumTargetTemperatureCelsius || celsius > kMaximumHotendTemperatureCelsius) return -EINVAL;
    return send(std::format("M104 S{:.1f}", celsius));
}

IOResult GcodePrinter::setBed(double celsius) {
    if (celsius < kMinimumTargetTemperatureCelsius || celsius > kMaximumBedTemperatureCelsius) return -EINVAL;
    return send(std::format("M140 S{:.1f}", celsius));
}

IOResult GcodePrinter::home() {
    return send("G28");
}

IOResult GcodePrinter::emergencyStop() {
    const bool saved = options_.lineNumbers;
    options_.lineNumbers = false;
    const auto rc = serial_.writeAll(handle_, "M112\n");
    options_.lineNumbers = saved;
    return rc;
}

}
