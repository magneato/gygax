#pragma once

#include <chrono>
#include <functional>
#include <istream>
#include <optional>
#include <string>

#include <gygax/transport/serial.hpp>

namespace gygax::transport {

inline constexpr std::chrono::milliseconds kDefaultGcodeCommandTimeout{15000};
inline constexpr std::chrono::milliseconds kDefaultGcodeBannerTimeout{2000};
inline constexpr int kDefaultGcodeMaxResends = 3;

struct Temperatures {
    double hotend = 0.0;
    double hotendTarget = 0.0;
    double bed = 0.0;
    double bedTarget = 0.0;
};

struct Position {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double e = 0.0;
};

struct GcodeOptions {
    bool lineNumbers = true;
    std::chrono::milliseconds commandTimeout = kDefaultGcodeCommandTimeout;
    int maxResends = kDefaultGcodeMaxResends;
};

class GcodePrinter {
public:
    using Progress = std::function<void(std::size_t sent, std::size_t total)>;

    GcodePrinter(SerialTransport& serial, TransportHandle handle, GcodeOptions options = {});

    [[nodiscard]] IOResult connect(std::chrono::milliseconds banner = kDefaultGcodeBannerTimeout);
    [[nodiscard]] IOResult send(const std::string& command, std::string* response = nullptr);
    [[nodiscard]] IOResult stream(std::istream& program, const Progress& progress = nullptr);
    [[nodiscard]] IOResult readTemperatures(Temperatures& out);
    [[nodiscard]] IOResult readPosition(Position& out);
    [[nodiscard]] IOResult setHotend(double celsius);
    [[nodiscard]] IOResult setBed(double celsius);
    [[nodiscard]] IOResult home();
    [[nodiscard]] IOResult emergencyStop();

    static std::optional<Temperatures> parseTemperatures(const std::string& text);
    static std::optional<Position> parsePosition(const std::string& text);
    static int checksum(const std::string& text);
    static std::string stripComment(const std::string& line);

private:
    SerialTransport& serial_;
    TransportHandle handle_;
    GcodeOptions options_;
    int nextLine_ = 1;
};

}
