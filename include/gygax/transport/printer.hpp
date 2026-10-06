#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include <gygax/transport/base.hpp>

namespace gygax::transport {

inline constexpr std::uint16_t kDefaultRawPrinterPort = 9100;
inline constexpr std::chrono::milliseconds kDefaultRawPrinterTimeout{10000};

struct PrinterTarget {
    enum class Kind { Tcp, Device };
    Kind kind = Kind::Tcp;
    std::string host;
    std::uint16_t port = kDefaultRawPrinterPort;
    std::string path;

    static bool parse(std::string_view spec, PrinterTarget& out, std::string& error);
};

class RawPrinter {
public:
    explicit RawPrinter(PrinterTarget target, std::chrono::milliseconds timeout = kDefaultRawPrinterTimeout)
        : target_(std::move(target)), timeout_(timeout) {}

    [[nodiscard]] IOResult print(std::string_view data) const;
    [[nodiscard]] IOResult printPjl(std::string_view jobName, std::string_view data) const;
    [[nodiscard]] IOResult query(std::string_view request, std::string& response) const;

    static std::string wrapPjl(std::string_view jobName, std::string_view data);

private:
    PrinterTarget target_;
    std::chrono::milliseconds timeout_;
};

}
