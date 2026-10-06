#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include <gygax/net/websocket.hpp>
#include <gygax/robotics/rosbridge.hpp>

#include "support.hpp"

using namespace gygax;
using namespace gygax::net;
using namespace std::chrono_literals;

namespace {

std::vector<std::uint8_t> bytesOf(const std::string& s) {
    return {s.begin(), s.end()};
}

std::vector<std::uint8_t> hexBytes(const std::string& hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

class TestServer {
public:
    using Handler = std::function<void(TestServer&, const WsMessage&)>;

    TestServer(std::shared_ptr<ByteLink> link, Handler handler) : link_(std::move(link)), handler_(std::move(handler)) {
        thread_ = std::jthread([this](const std::stop_token& st) { run(st); });
    }

    void sendText(const std::string& text) { write(encodeWebSocketFrame(WsOpcode::Text, text, false)); }
    void write(const std::string& frame) { (void)link_->write({reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()}); }
    std::atomic<int> handshakes{0};
    std::atomic<int> pongs{0};
    std::atomic<bool> gotClose{false};

private:
    void run(const std::stop_token& st) {
        std::string request;
        std::vector<std::uint8_t> chunk;
        while (!st.stop_requested() && request.find("\r\n\r\n") == std::string::npos) {
            if (link_->read(chunk, 20ms) == 0) request.append(chunk.begin(), chunk.end());
        }
        if (st.stop_requested()) return;
        const auto response = webSocketServerResponse(request);
        if (!response) return;
        ++handshakes; // before the reply, so the client never sees the response first
        write(*response);
        WebSocketParser parser;
        while (!st.stop_requested()) {
            if (link_->read(chunk, 20ms) != 0) continue;
            for (const auto& m : parser.feed(chunk)) {
                if (m.opcode == WsOpcode::Close) {
                    gotClose = true;
                    write(encodeWebSocketFrame(WsOpcode::Close, m.payload, false));
                } else if (m.opcode == WsOpcode::Pong) {
                    ++pongs;
                } else {
                    handler_(*this, m);
                }
            }
        }
    }

    std::shared_ptr<ByteLink> link_;
    Handler handler_;
    std::jthread thread_;
};

} // namespace

TEST(Crypto, Sha1Base64AndAcceptKeyMatchPublishedVectors) {
    auto hex = [](const std::string& raw) {
        std::string out;
        for (const unsigned char c : raw) out += std::format("{:02x}", c);
        return out;
    };
    EXPECT_EQ(hex(sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_EQ(hex(sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(hex(sha1("The quick brown fox jumps over the lazy dog")), "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
    EXPECT_EQ(hex(sha1(std::string(1000, 'a'))), "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    EXPECT_EQ(base64Encode("Man"), "TWFu");
    EXPECT_EQ(base64Encode("Ma"), "TWE=");
    EXPECT_EQ(base64Encode("M"), "TQ==");
    EXPECT_EQ(base64Encode(""), "");
    EXPECT_EQ(*base64Decode("TWFu"), "Man");
    EXPECT_EQ(*base64Decode("TWE="), "Ma");
    EXPECT_FALSE(base64Decode("TWF"));
    EXPECT_FALSE(base64Decode("T=Fu"));
    EXPECT_FALSE(base64Decode("TW!u"));
    EXPECT_EQ(webSocketAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WebSocketFrames, EncodeAndParseRfcExamples) {
    EXPECT_EQ(encodeWebSocketFrame(WsOpcode::Text, "Hello", false), std::string("\x81\x05Hello"));
    const auto masked = encodeWebSocketFrame(WsOpcode::Text, "Hello", true, true, 0x37FA213D);
    const auto expected = hexBytes("818537fa213d7f9f4d5158");
    EXPECT_EQ(masked, std::string(expected.begin(), expected.end()));
    WebSocketParser parser;
    auto messages = parser.feed(expected);
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_EQ(messages[0].payload, "Hello");

    const auto part1 = hexBytes("010348656c");
    const auto part2 = hexBytes("80026c6f");
    EXPECT_TRUE(parser.feed(part1).empty());
    messages = parser.feed(part2);
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_EQ(messages[0].payload, "Hello");
}

TEST(WebSocketFrames, LargePayloadsSplitAcrossReads) {
    const std::string big(70000, 'x');
    const auto frame = encodeWebSocketFrame(WsOpcode::Binary, big, true);
    WebSocketParser parser;
    std::vector<WsMessage> all;
    for (std::size_t i = 0; i < frame.size(); i += 1000) {
        const auto chunk = bytesOf(frame.substr(i, 1000));
        auto got = parser.feed(chunk);
        all.insert(all.end(), got.begin(), got.end());
    }
    ASSERT_EQ(all.size(), 1U);
    EXPECT_EQ(all[0].payload, big);
    EXPECT_EQ(all[0].opcode, WsOpcode::Binary);
}

TEST(WebSocketFrames, ProtocolViolationsFailTheParser) {
    WebSocketParser reserved;
    (void)reserved.feed(hexBytes("c10100"));
    EXPECT_TRUE(reserved.failed());
    WebSocketParser orphan;
    (void)orphan.feed(hexBytes("80026c6f"));
    EXPECT_TRUE(orphan.failed());
    WebSocketParser fragmentedControl;
    (void)fragmentedControl.feed(hexBytes("090100"));
    EXPECT_TRUE(fragmentedControl.failed());
    WebSocketParser tooBig(100);
    (void)tooBig.feed(hexBytes("827e0100"));
    EXPECT_TRUE(tooBig.failed());
    WebSocketParser badOpcode;
    (void)badOpcode.feed(hexBytes("8301ff"));
    EXPECT_TRUE(badOpcode.failed());
}

TEST(WebSocketUrl, ParsingAndRejections) {
    auto u = WebSocketUrl::parse("ws://robot.local:9090/rosbridge");
    ASSERT_TRUE(u);
    EXPECT_EQ(u->host, "robot.local");
    EXPECT_EQ(u->port, 9090);
    EXPECT_EQ(u->path, "/rosbridge");
    EXPECT_EQ(WebSocketUrl::parse("ws://host")->port, 80);
    std::string error;
    EXPECT_FALSE(WebSocketUrl::parse("wss://host", &error));
    EXPECT_NE(error.find("TLS"), std::string::npos);
    EXPECT_FALSE(WebSocketUrl::parse("http://host", &error));
    EXPECT_FALSE(WebSocketUrl::parse("ws://:9090", &error));
    EXPECT_FALSE(WebSocketUrl::parse("ws://host:0", &error));
}

TEST(WebSocketClient, HandshakeEchoPingAndClose) {
    auto [clientLink, serverLink] = makeLinkPair();
    TestServer server(serverLink, [](TestServer& s, const WsMessage& m) { s.sendText("echo:" + m.payload); });
    std::string error;
    auto ws = WebSocketClient::attach(clientLink, "test", "/", 2s, &error);
    ASSERT_TRUE(ws) << error;
    EXPECT_EQ(server.handshakes.load(), 1);
    ASSERT_EQ(ws->sendText("hello"), 0);
    WsMessage m;
    ASSERT_EQ(ws->receive(m, 2s), 0);
    EXPECT_EQ(m.payload, "echo:hello");
    const std::string big(100000, 'z');
    ASSERT_EQ(ws->sendText(big), 0);
    ASSERT_EQ(ws->receive(m, 2s), 0);
    EXPECT_EQ(m.payload.size(), big.size() + 5);
    server.write(encodeWebSocketFrame(WsOpcode::Ping, "beat", false));
    server.sendText("after-ping");
    ASSERT_EQ(ws->receive(m, 2s), 0);
    EXPECT_EQ(m.payload, "after-ping");
    ASSERT_TRUE(support::waitUntil([&] { return server.pongs.load() == 1; }));
    EXPECT_EQ(ws->receive(m, 30ms), -ETIMEDOUT);
    ASSERT_EQ(ws->close(), 0);
    EXPECT_TRUE(support::waitUntil([&] { return server.gotClose.load(); }));
    EXPECT_EQ(ws->sendText("late"), -ECONNRESET);
}

TEST(WebSocketClient, RejectsBadServers) {
    {
        auto [clientLink, serverLink] = makeLinkPair();
        std::jthread bad([&, serverLink](const std::stop_token&) {
            std::vector<std::uint8_t> chunk;
            (void)serverLink->read(chunk, 1s);
            const std::string reply =
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: bogus\r\n\r\n";
            (void)serverLink->write(bytesOf(reply));
        });
        std::string error;
        EXPECT_FALSE(WebSocketClient::attach(clientLink, "test", "/", 1s, &error));
        EXPECT_NE(error.find("Sec-WebSocket-Accept"), std::string::npos);
    }
    {
        auto [clientLink, serverLink] = makeLinkPair();
        std::jthread refuse([&, serverLink](const std::stop_token&) {
            std::vector<std::uint8_t> chunk;
            (void)serverLink->read(chunk, 1s);
            (void)serverLink->write(bytesOf("HTTP/1.1 404 Not Found\r\n\r\n"));
        });
        std::string error;
        EXPECT_FALSE(WebSocketClient::attach(clientLink, "test", "/", 1s, &error));
        EXPECT_NE(error.find("404"), std::string::npos);
    }
    {
        auto [clientLink, serverLink] = makeLinkPair();
        std::string error;
        EXPECT_FALSE(WebSocketClient::attach(clientLink, "test", "/", 100ms, &error));
        EXPECT_NE(error.find("timed out"), std::string::npos);
    }
}

namespace {

struct RosRig {
    RosRig() {
        auto [clientLink, serverLink] = makeLinkPair();
        server = std::make_unique<TestServer>(serverLink, [this](TestServer& s, const WsMessage& m) {
            auto msg = json::parse(m.payload);
            if (!msg) return;
            std::lock_guard lock(mutex);
            received.push_back(*msg);
            const auto op = msg->getString("op");
            if (op == "subscribe") {
                json::Value out = json::Value::object();
                out["op"] = "publish";
                out["topic"] = msg->getString("topic");
                out["msg"]["data"] = "hello from the robot";
                out["msg"]["seq"] = 1;
                s.sendText(out.dump());
            } else if (op == "call_service") {
                json::Value out = json::Value::object();
                out["op"] = "service_response";
                out["service"] = msg->getString("service");
                out["id"] = msg->getString("id");
                if (msg->getString("service") == "/fail") {
                    out["result"] = false;
                    out["values"] = "no such thing";
                } else if (msg->getString("service") == "/never") {
                    return;
                } else {
                    out["result"] = true;
                    out["values"]["topics"].push("/cmd_vel");
                    out["values"]["topics"].push("/odom");
                }
                s.sendText(out.dump());
            }
        });
        std::string error;
        auto ws = WebSocketClient::attach(clientLink, "test", "/", 2s, &error);
        EXPECT_TRUE(ws) << error;
        client = std::make_unique<robotics::RosbridgeClient>(std::move(ws));
        client->start();
    }

    json::Value nth(std::size_t i) {
        std::lock_guard lock(mutex);
        return i < received.size() ? received[i] : json::Value();
    }

    std::size_t count() {
        std::lock_guard lock(mutex);
        return received.size();
    }

    std::mutex mutex;
    std::vector<json::Value> received;
    std::unique_ptr<TestServer> server;
    std::unique_ptr<robotics::RosbridgeClient> client;
};

} // namespace

TEST(Rosbridge, PublishesTwistsAndAdvertisesTopics) {
    RosRig rig;
    ASSERT_EQ(rig.client->advertise("/cmd_vel", "geometry_msgs/Twist"), 0);
    ASSERT_EQ(rig.client->publishTwist("/cmd_vel", 0.5, -0.25), 0);
    ASSERT_TRUE(support::waitUntil([&] { return rig.count() >= 2; }));
    const auto adv = rig.nth(0);
    EXPECT_EQ(adv.getString("op"), "advertise");
    EXPECT_EQ(adv.getString("type"), "geometry_msgs/Twist");
    const auto pub = rig.nth(1);
    EXPECT_EQ(pub.getString("op"), "publish");
    EXPECT_DOUBLE_EQ(pub.find("msg")->find("linear")->getDouble("x"), 0.5);
    EXPECT_DOUBLE_EQ(pub.find("msg")->find("angular")->getDouble("z"), -0.25);
    EXPECT_EQ(rig.client->advertise("", "x"), -EINVAL);
    EXPECT_EQ(rig.client->publish("/x", *json::parse("[1]")), -EINVAL);
    ASSERT_EQ(rig.client->unadvertise("/cmd_vel"), 0);
}

TEST(Rosbridge, SubscribeDeliversMessagesToHandlersAndCachesTheLast) {
    RosRig rig;
    std::atomic<int> calls{0};
    std::string seen;
    std::mutex m;
    ASSERT_EQ(rig.client->subscribe(
                  "/status", "std_msgs/String",
                  [&](const std::string& topic, const json::Value& msg) {
                      std::lock_guard lock(m);
                      seen = topic + ":" + msg.getString("data");
                      ++calls;
                  },
                  100),
              0);
    ASSERT_TRUE(support::waitUntil([&] { return calls.load() == 1; }));
    {
        std::lock_guard lock(m);
        EXPECT_EQ(seen, "/status:hello from the robot");
    }
    EXPECT_EQ(rig.client->lastMessage("/status")->getString("data"), "hello from the robot");
    EXPECT_FALSE(rig.client->lastMessage("/other"));
    EXPECT_EQ(rig.client->subscribedTopics(), std::vector<std::string>{"/status"});
    const auto sub = rig.nth(0);
    EXPECT_EQ(sub.getInt("throttle_rate"), 100);
    ASSERT_EQ(rig.client->unsubscribe("/status"), 0);
    EXPECT_TRUE(rig.client->subscribedTopics().empty());
}

TEST(Rosbridge, ServiceCallsReturnValuesErrorsAndTimeouts) {
    RosRig rig;
    json::Value result;
    ASSERT_EQ(rig.client->callService("/rosapi/topics", json::Value::object(), result, 2s), 0);
    ASSERT_EQ(result.find("topics")->asArray().size(), 2U);
    EXPECT_EQ(result.find("topics")->asArray()[0].asString(), "/cmd_vel");
    EXPECT_EQ(rig.client->callService("/fail", json::Value::object(), result, 2s), -EREMOTEIO);
    EXPECT_EQ(rig.client->lastError(), "no such thing");
    EXPECT_EQ(rig.client->callService("/never", json::Value::object(), result, 100ms), -ETIMEDOUT);
    EXPECT_EQ(rig.client->callService("", json::Value::object(), result), -EINVAL);
}

TEST(Rosbridge, ReportsALostConnection) {
    RosRig rig;
    EXPECT_TRUE(rig.client->connected());
    rig.server->write(encodeWebSocketFrame(WsOpcode::Close, std::string("\x03\xE8", 2), false));
    ASSERT_TRUE(support::waitUntil([&] { return !rig.client->connected(); }));
    EXPECT_EQ(rig.client->publishTwist("/cmd_vel", 1, 0), -ECONNRESET);
    json::Value result;
    EXPECT_EQ(rig.client->callService("/rosapi/topics", json::Value::object(), result, 200ms), -ECONNRESET);
}

TEST(Rosbridge, ConnectFailsCleanlyWithoutAServer) {
    std::string error;
    EXPECT_EQ(robotics::RosbridgeClient::connect("ws://127.0.0.1:1/", 200ms, &error), nullptr);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(robotics::RosbridgeClient::connect("wss://robot/", 200ms, &error), nullptr);
}
