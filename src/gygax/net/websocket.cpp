#include <gygax/net/websocket.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <deque>
#include <format>
#include <random>

#include <gygax/net/http.hpp>

namespace gygax::net {

namespace {

constexpr std::size_t kBaseFrameHeaderBytes = 2;
constexpr std::size_t kExtended16BitFrameHeaderBytes = 4;
constexpr std::size_t kExtended64BitFrameHeaderBytes = 10;
constexpr std::size_t kExtended64BitLengthBytes = 8;
constexpr std::size_t kWebSocketMaskKeyBytes = 4;
constexpr std::size_t kWebSocketNonceBytes = 16;
constexpr std::size_t kMaximumControlFramePayloadBytes = 125;
constexpr std::size_t kMaximumHandshakeResponseBytes = 16 * 1024;
constexpr std::uint8_t kFinalFrameBit = 0x80;
constexpr std::uint8_t kMaskFlagBit = 0x80;
constexpr std::uint8_t kReservedBitsMask = 0x70;
constexpr std::uint8_t kOpcodeMask = 0x0F;
constexpr std::uint8_t kShortPayloadLengthMask = 0x7F;
constexpr std::uint8_t kExtended16BitLengthMarker = 126;
constexpr std::uint8_t kExtended64BitLengthMarker = 127;
constexpr std::size_t kMaximumDirectPayloadLength = kExtended16BitLengthMarker - 1;
constexpr std::uint64_t kMaximumExtended16BitPayloadLength = 0xFFFF;
constexpr unsigned kMaximumPortNumber = 65535;
constexpr int kWebSocketCloseNormalStatus = 1000;
constexpr std::array<std::uint8_t, 2> kWebSocketCloseNormalPayload = {static_cast<std::uint8_t>(kWebSocketCloseNormalStatus >> 8),
                                                                      static_cast<std::uint8_t>(kWebSocketCloseNormalStatus & 0xFF)};
constexpr std::string_view kWebSocketAcceptGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::string_view kWebSocketHeaderTerminator = "\r\n\r\n";

std::uint32_t rotl(std::uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return out;
}

} // namespace

std::string sha1(std::string_view data) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string msg(data);
    const std::uint64_t bitLength = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitLength >> (8 * i)) & 0xFF));
    for (std::size_t offset = 0; offset < msg.size(); offset += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(msg.data()) + offset + static_cast<std::size_t>(i) * 4;
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0];
        std::uint32_t b = h[1];
        std::uint32_t c = h[2];
        std::uint32_t d = h[3];
        std::uint32_t e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f;
            std::uint32_t k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::string out;
    for (const auto word : h) {
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((word >> (8 * i)) & 0xFF));
    }
    return out;
}

std::string base64Encode(std::string_view data) {
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint8_t>(data[i]) << 16) | (static_cast<std::uint8_t>(data[i + 1]) << 8) |
                                static_cast<std::uint8_t>(data[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i + 1 == data.size()) {
        const std::uint32_t v = static_cast<std::uint8_t>(data[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        const std::uint32_t v = (static_cast<std::uint8_t>(data[i]) << 16) | (static_cast<std::uint8_t>(data[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;
    std::string out;
    for (std::size_t i = 0; i < text.size(); i += 4) {
        std::uint32_t v = 0;
        int pad = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = text[i + static_cast<std::size_t>(j)];
            if (c == '=') {
                if (i + 4 != text.size() || j < 2) return std::nullopt;
                ++pad;
                v <<= 6;
                continue;
            }
            if (pad > 0) return std::nullopt;
            const auto pos = kAlphabet.find(c);
            if (pos == std::string_view::npos) return std::nullopt;
            v = (v << 6) | static_cast<std::uint32_t>(pos);
        }
        out.push_back(static_cast<char>((v >> 16) & 0xFF));
        if (pad < 2) out.push_back(static_cast<char>((v >> 8) & 0xFF));
        if (pad < 1) out.push_back(static_cast<char>(v & 0xFF));
    }
    return out;
}

std::string webSocketAcceptKey(std::string_view clientKey) {
    return base64Encode(sha1(std::string(clientKey) + std::string(kWebSocketAcceptGuid)));
}

std::string encodeWebSocketFrame(WsOpcode opcode, std::string_view payload, bool mask, bool fin, std::uint32_t maskKey) {
    std::string out;
    out.push_back(static_cast<char>((fin ? kFinalFrameBit : 0x00) | static_cast<std::uint8_t>(opcode)));
    const std::uint8_t maskBit = mask ? kMaskFlagBit : 0x00;
    if (payload.size() <= kMaximumDirectPayloadLength) {
        out.push_back(static_cast<char>(maskBit | payload.size()));
    } else if (payload.size() <= kMaximumExtended16BitPayloadLength) {
        out.push_back(static_cast<char>(maskBit | kExtended16BitLengthMarker));
        out.push_back(static_cast<char>(payload.size() >> 8));
        out.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        out.push_back(static_cast<char>(maskBit | 127));
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (8 * i)) & 0xFF));
    }
    if (mask) {
        if (maskKey == 0) {
            static thread_local std::mt19937 rng{std::random_device{}()};
            maskKey = static_cast<std::uint32_t>(rng());
        }
        const std::array<std::uint8_t, kWebSocketMaskKeyBytes> key = {
            static_cast<std::uint8_t>(maskKey >> 24), static_cast<std::uint8_t>(maskKey >> 16), static_cast<std::uint8_t>(maskKey >> 8),
            static_cast<std::uint8_t>(maskKey)};
        for (const auto b : key) out.push_back(static_cast<char>(b));
        for (std::size_t i = 0; i < payload.size(); ++i)
            out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i % kWebSocketMaskKeyBytes]));
    } else {
        out.append(payload);
    }
    return out;
}

std::vector<WsMessage> WebSocketParser::feed(std::span<const std::uint8_t> data) {
    std::vector<WsMessage> out;
    if (failed_) return out;
    buffer_.append(reinterpret_cast<const char*>(data.data()), data.size());
    std::size_t pos = 0;
    while (true) {
        if (buffer_.size() - pos < kBaseFrameHeaderBytes) break;
        const auto b0 = static_cast<std::uint8_t>(buffer_[pos]);
        const auto b1 = static_cast<std::uint8_t>(buffer_[pos + 1]);
        if ((b0 & kReservedBitsMask) != 0) {
            failed_ = true;
            break;
        }
        const bool fin = (b0 & kFinalFrameBit) != 0;
        const auto opcode = static_cast<WsOpcode>(b0 & kOpcodeMask);
        const bool masked = (b1 & kMaskFlagBit) != 0;
        std::uint64_t length = b1 & kShortPayloadLengthMask;
        std::size_t header = kBaseFrameHeaderBytes;
        if (length == kExtended16BitLengthMarker) {
            if (buffer_.size() - pos < kExtended16BitFrameHeaderBytes) break;
            length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer_[pos + 2])) << 8) |
                     static_cast<std::uint8_t>(buffer_[pos + 3]);
            header = kExtended16BitFrameHeaderBytes;
        } else if (length == kExtended64BitLengthMarker) {
            if (buffer_.size() - pos < kExtended64BitFrameHeaderBytes) break;
            length = 0;
            for (std::size_t i = 0; i < kExtended64BitLengthBytes; ++i)
                length = (length << 8) | static_cast<std::uint8_t>(buffer_[pos + kBaseFrameHeaderBytes + i]);
            header = kExtended64BitFrameHeaderBytes;
        }
        if (length > maxMessage_) {
            failed_ = true;
            break;
        }
        const std::size_t maskLen = masked ? kWebSocketMaskKeyBytes : 0;
        if (buffer_.size() - pos < header + maskLen + length) break;
        std::string payload = buffer_.substr(pos + header + maskLen, static_cast<std::size_t>(length));
        if (masked) {
            for (std::size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^
                                               static_cast<std::uint8_t>(buffer_[pos + header + i % kWebSocketMaskKeyBytes]));
        }
        pos += header + maskLen + static_cast<std::size_t>(length);
        const bool control = static_cast<std::uint8_t>(opcode) >= 8;
        if (control) {
            if (!fin || payload.size() > kMaximumControlFramePayloadBytes) {
                failed_ = true;
                break;
            }
            out.push_back({opcode, std::move(payload)});
            continue;
        }
        if (opcode == WsOpcode::Continuation) {
            if (!inFragment_) {
                failed_ = true;
                break;
            }
            fragments_ += payload;
            if (fragments_.size() > maxMessage_) {
                failed_ = true;
                break;
            }
            if (fin) {
                out.push_back({fragmentOpcode_, std::move(fragments_)});
                fragments_.clear();
                inFragment_ = false;
            }
        } else if (opcode == WsOpcode::Text || opcode == WsOpcode::Binary) {
            if (inFragment_) {
                failed_ = true;
                break;
            }
            if (fin) {
                out.push_back({opcode, std::move(payload)});
            } else {
                inFragment_ = true;
                fragmentOpcode_ = opcode;
                fragments_ = std::move(payload);
            }
        } else {
            failed_ = true;
            break;
        }
    }
    buffer_.erase(0, pos);
    return out;
}

std::optional<WebSocketUrl> WebSocketUrl::parse(std::string_view url, std::string* error) {
    auto fail = [&](const char* m) -> std::optional<WebSocketUrl> {
        if (error != nullptr) *error = m;
        return std::nullopt;
    };
    WebSocketUrl out;
    if (url.starts_with("wss://")) return fail("wss:// is not supported; terminate TLS in a local proxy and use ws://");
    if (!url.starts_with("ws://")) return fail("expected a ws:// url");
    url.remove_prefix(std::string_view("ws://").size());
    const auto slash = url.find('/');
    auto authority = url.substr(0, slash);
    out.path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        unsigned port = 0;
        const auto text = authority.substr(colon + 1);
        if (std::from_chars(text.data(), text.data() + text.size(), port).ec != std::errc() || port == 0 || port > kMaximumPortNumber)
            return fail("invalid port");
        out.port = static_cast<std::uint16_t>(port);
        authority = authority.substr(0, colon);
    }
    if (authority.empty()) return fail("missing host");
    out.host = std::string(authority);
    return out;
}

std::unique_ptr<WebSocketClient> WebSocketClient::connect(std::string_view url, std::chrono::milliseconds timeout, std::string* error) {
    auto parsed = WebSocketUrl::parse(url, error);
    if (!parsed) return nullptr;
    auto link = openTcpLink(parsed->host, parsed->port, timeout, error);
    if (!link) return nullptr;
    return attach(std::move(link), std::format("{}:{}", parsed->host, parsed->port), parsed->path, timeout, error);
}

std::unique_ptr<WebSocketClient> WebSocketClient::attach(std::shared_ptr<ByteLink> link, const std::string& host, const std::string& path,
                                                         std::chrono::milliseconds timeout, std::string* error) {
    auto fail = [&](std::string message) -> std::unique_ptr<WebSocketClient> {
        if (error != nullptr) *error = std::move(message);
        return nullptr;
    };
    std::string keyBytes(kWebSocketNonceBytes, '\0');
    std::random_device rd;
    for (auto& c : keyBytes) c = static_cast<char>(rd());
    const std::string key = base64Encode(keyBytes);
    const std::string request =
        std::format("GET {} HTTP/1.1\r\nHost: {}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {}\r\n"
                    "Sec-WebSocket-Version: 13\r\n\r\n",
                    path, host, key);
    if (link->write({reinterpret_cast<const std::uint8_t*>(request.data()), request.size()}) != 0)
        return fail("cannot send the WebSocket handshake");
    std::string response;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t end = std::string::npos;
    while (end == std::string::npos) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return fail("timed out waiting for the WebSocket handshake");
        std::vector<std::uint8_t> chunk;
        const auto rc =
            link->read(chunk, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) + std::chrono::milliseconds(1));
        if (rc == -ETIMEDOUT) continue;
        if (rc != 0) return fail("connection closed during the WebSocket handshake");
        response.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        end = response.find(kWebSocketHeaderTerminator);
        if (response.size() > kMaximumHandshakeResponseBytes) return fail("oversized handshake response");
    }
    const std::string head = response.substr(0, end);
    const auto upgradeStatusLine = std::format("HTTP/1.1 {} ", kHttpStatusSwitchingProtocols);
    if (!head.starts_with(upgradeStatusLine)) return fail("server refused the upgrade: " + head.substr(0, head.find("\r\n")));
    const std::string expected = webSocketAcceptKey(key);
    std::string accept;
    std::size_t line = head.find("\r\n");
    while (line != std::string::npos) {
        const auto next = head.find("\r\n", line + 2);
        const std::string header = head.substr(line + 2, next == std::string::npos ? std::string::npos : next - line - 2);
        if (lower(header).rfind("sec-websocket-accept:", 0) == 0) {
            accept = header.substr(header.find(':') + 1);
            accept.erase(0, accept.find_first_not_of(' '));
        }
        line = next;
    }
    if (accept != expected) return fail("invalid Sec-WebSocket-Accept from the server");
    return std::unique_ptr<WebSocketClient>(new WebSocketClient(std::move(link), response.substr(end + 4)));
}

IOResult WebSocketClient::sendFrame(WsOpcode opcode, std::string_view payload) {
    if (closed_) return -ECONNRESET;
    const auto frame = encodeWebSocketFrame(opcode, payload, true);
    std::lock_guard lock(writeMutex_);
    return link_->write({reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()});
}

IOResult WebSocketClient::sendText(std::string_view text) {
    return sendFrame(WsOpcode::Text, text);
}
IOResult WebSocketClient::sendBinary(std::string_view data) {
    return sendFrame(WsOpcode::Binary, data);
}

IOResult WebSocketClient::close() {
    if (closed_) return 0;
    const auto rc = sendFrame(WsOpcode::Close, std::string_view(reinterpret_cast<const char*>(kWebSocketCloseNormalPayload.data()),
                                                                kWebSocketCloseNormalPayload.size()));
    closed_ = true;
    return rc;
}

IOResult WebSocketClient::receive(WsMessage& message, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (!pendingBytes_.empty()) {
            auto more = parser_.feed({reinterpret_cast<const std::uint8_t*>(pendingBytes_.data()), pendingBytes_.size()});
            pendingBytes_.clear();
            ready_.insert(ready_.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
        }
        while (!ready_.empty()) {
            WsMessage m = std::move(ready_.front());
            ready_.erase(ready_.begin());
            if (m.opcode == WsOpcode::Ping) {
                (void)sendFrame(WsOpcode::Pong, m.payload);
                continue;
            }
            if (m.opcode == WsOpcode::Pong) continue;
            if (m.opcode == WsOpcode::Close) {
                if (!closed_) (void)sendFrame(WsOpcode::Close, m.payload);
                closed_ = true;
                return -ECONNRESET;
            }
            message = std::move(m);
            return 0;
        }
        if (parser_.failed()) return -EPROTO;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return -ETIMEDOUT;
        std::vector<std::uint8_t> chunk;
        const auto rc =
            link_->read(chunk, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) + std::chrono::milliseconds(1));
        if (rc == -ETIMEDOUT) continue;
        if (rc != 0) {
            closed_ = true;
            return rc;
        }
        auto more = parser_.feed(chunk);
        ready_.insert(ready_.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
    }
}

std::optional<std::string> webSocketServerResponse(std::string_view request) {
    const auto keyPos = lower(request).find("sec-websocket-key:");
    if (keyPos == std::string::npos) return std::nullopt;
    auto start = request.find(':', keyPos) + 1;
    while (start < request.size() && request[start] == ' ') ++start;
    const auto end = request.find("\r\n", start);
    const std::string key(request.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    return std::format("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
                       webSocketAcceptKey(key));
}

} // namespace gygax::net
