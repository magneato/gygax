#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace gygax::comm {

inline constexpr const char* kDefaultUdpBindHost = "127.0.0.1";

class Stream {
public:
    virtual ~Stream() = default;
    [[nodiscard]] virtual bool transmit(const std::string& destination, const std::string& message) = 0;
    [[nodiscard]] virtual std::optional<std::string> receive() = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};

[[nodiscard]] std::unique_ptr<Stream> CreateUdpStream(std::uint16_t bindPort = 0, const std::string& bindHost = kDefaultUdpBindHost);
[[nodiscard]] std::unique_ptr<Stream> CreateTcpLineStream();

}
