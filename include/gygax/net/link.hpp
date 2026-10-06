#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gygax/transport/base.hpp>

namespace gygax::net {

using transport::IOResult;

class ByteLink {
public:
    virtual ~ByteLink() = default;
    [[nodiscard]] virtual IOResult write(std::span<const std::uint8_t> data) = 0;
    [[nodiscard]] virtual IOResult read(std::vector<std::uint8_t>& out, std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual std::string describe() const = 0;
};

std::pair<std::shared_ptr<ByteLink>, std::shared_ptr<ByteLink>> makeLinkPair();

std::shared_ptr<ByteLink> openUdpLink(const std::string& bindHost, std::uint16_t bindPort, const std::string& remoteHost,
                                      std::uint16_t remotePort, std::string* error = nullptr);
std::shared_ptr<ByteLink> openTcpLink(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout,
                                      std::string* error = nullptr);
std::shared_ptr<ByteLink> openSerialLink(const std::string& device, std::uint32_t baud, std::string* error = nullptr);

std::shared_ptr<ByteLink> openLink(std::string_view uri, std::string* error = nullptr);

std::shared_ptr<ByteLink> memoryLinkPeer(const std::string& name);
void resetMemoryLinks();

} // namespace gygax::net
