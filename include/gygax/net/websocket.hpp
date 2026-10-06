#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gygax/net/link.hpp>

namespace gygax::net {

inline constexpr std::size_t kDefaultMaximumWebSocketMessageBytes = 16 * 1024 * 1024;
inline constexpr std::uint16_t kDefaultWebSocketPort = 80;

std::string sha1(std::string_view data);
std::string base64Encode(std::string_view data);
std::optional<std::string> base64Decode(std::string_view text);
std::string webSocketAcceptKey(std::string_view clientKey);

enum class WsOpcode : std::uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

struct WsMessage {
    WsOpcode opcode = WsOpcode::Text;
    std::string payload;
};

std::string encodeWebSocketFrame(WsOpcode opcode, std::string_view payload, bool mask, bool fin = true, std::uint32_t maskKey = 0);

class WebSocketParser {
public:
    explicit WebSocketParser(std::size_t maxMessage = kDefaultMaximumWebSocketMessageBytes) : maxMessage_(maxMessage) {}

    std::vector<WsMessage> feed(std::span<const std::uint8_t> data);
    [[nodiscard]] bool failed() const { return failed_; }

private:
    std::string buffer_;
    std::string fragments_;
    WsOpcode fragmentOpcode_ = WsOpcode::Text;
    bool inFragment_ = false;
    bool failed_ = false;
    std::size_t maxMessage_;
};

struct WebSocketUrl {
    std::string host;
    std::uint16_t port = kDefaultWebSocketPort;
    std::string path = "/";
    static std::optional<WebSocketUrl> parse(std::string_view url, std::string* error = nullptr);
};

class WebSocketClient {
public:
    static std::unique_ptr<WebSocketClient> connect(std::string_view url, std::chrono::milliseconds timeout, std::string* error = nullptr);
    static std::unique_ptr<WebSocketClient> attach(std::shared_ptr<ByteLink> link, const std::string& host, const std::string& path,
                                                   std::chrono::milliseconds timeout, std::string* error = nullptr);

    [[nodiscard]] IOResult sendText(std::string_view text);
    [[nodiscard]] IOResult sendBinary(std::string_view data);
    [[nodiscard]] IOResult receive(WsMessage& message, std::chrono::milliseconds timeout);
    [[nodiscard]] IOResult close();
    [[nodiscard]] bool closed() const { return closed_; }

private:
    WebSocketClient(std::shared_ptr<ByteLink> link, std::string leftover) : link_(std::move(link)), pendingBytes_(std::move(leftover)) {}

    [[nodiscard]] IOResult sendFrame(WsOpcode opcode, std::string_view payload);

    std::shared_ptr<ByteLink> link_;
    WebSocketParser parser_;
    std::string pendingBytes_;
    std::vector<WsMessage> ready_;
    std::mutex writeMutex_;
    bool closed_ = false;
};

std::optional<std::string> webSocketServerResponse(std::string_view request);

} // namespace gygax::net
