#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

#include <gygax/bus/modbus.hpp>
#include <gygax/bus/dbc.hpp>
#include <gygax/bus/j1939.hpp>
#include <gygax/bus/obd2.hpp>
#include <gygax/core/log.hpp>
#include <gygax/net/websocket.hpp>
#include <gygax/robotics/device.hpp>
#include <gygax/service/service.hpp>

#include "fake_autopilot.hpp"
#include "support.hpp"

using namespace gygax;
using namespace gygax::robotics;
using namespace std::chrono_literals;

namespace {

constexpr auto kDeviceReceiveTimeout = 500ms;

json::Value cfg(const char* text) {
    return *json::parse(text);
}

std::unique_ptr<Device> open(const char* text, std::string* error = nullptr) {
    std::string local;
    return openDevice(cfg(text), error != nullptr ? error : &local);
}

const char* kDbc = R"DBC(VERSION ""

NS_ :

BS_:

BU_: ECU

BO_ 500 Engine: 8 ECU
 SG_ EngineSpeed : 0|16@1+ (0.25,0) [0|16383.75] "rpm" Vector__XXX
 SG_ Gear : 36|4@1+ (1,0) [0|15] "" Vector__XXX

VAL_ 500 Gear 0 "P" 3 "D" ;
)DBC";

int listenOnLoopback(std::uint16_t& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
    EXPECT_EQ(::listen(fd, 4), 0);
    return fd;
}

class TcpRosServer {
public:
    TcpRosServer() {
        listenFd_ = listenOnLoopback(port);
        thread_ = std::jthread([this](const std::stop_token& st) { run(st); });
    }

    ~TcpRosServer() {
        thread_.request_stop();
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
    }

    std::uint16_t port = 0;
    std::mutex mutex;
    std::vector<json::Value> received;

private:
    void run(const std::stop_token& st) {
        const int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) return;
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return;
            request.append(buf, static_cast<std::size_t>(n));
        }
        const auto response = net::webSocketServerResponse(request);
        ::send(fd, response->data(), response->size(), MSG_NOSIGNAL);
        net::WebSocketParser parser;
        while (!st.stop_requested()) {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            for (const auto& m : parser.feed({reinterpret_cast<const std::uint8_t*>(buf), static_cast<std::size_t>(n)})) {
                auto msg = json::parse(m.payload);
                if (!msg) continue;
                {
                    std::lock_guard lock(mutex);
                    received.push_back(*msg);
                }
                if (msg->getString("op") == "subscribe") {
                    json::Value out = json::Value::object();
                    out["op"] = "publish";
                    out["topic"] = msg->getString("topic");
                    out["msg"]["x"] = 1.5;
                    const auto frame = net::encodeWebSocketFrame(net::WsOpcode::Text, out.dump(), false);
                    ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
                } else if (msg->getString("op") == "call_service") {
                    json::Value out = json::Value::object();
                    out["op"] = "service_response";
                    out["id"] = msg->getString("id");
                    out["result"] = true;
                    out["values"]["nodes"].push("/robot_state_publisher");
                    const auto frame = net::encodeWebSocketFrame(net::WsOpcode::Text, out.dump(), false);
                    ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
                }
            }
        }
        ::close(fd);
    }

    int listenFd_ = -1;
    std::jthread thread_;
};

} // namespace

TEST(DeviceConfig, RejectsBadConfigurationsWithClearErrors) {
    std::string error;
    EXPECT_EQ(open(R"({"kind":"warp-drive"})", &error), nullptr);
    EXPECT_NE(error.find("unknown device kind"), std::string::npos);
    EXPECT_EQ(open(R"({"kind":"mavlink"})", &error), nullptr);
    EXPECT_EQ(open(R"({"kind":"mavlink","uri":"serial:///etc/passwd"})", &error), nullptr);
    EXPECT_NE(error.find("/dev/"), std::string::npos);
    EXPECT_EQ(open(R"({"kind":"mavlink","uri":"serial:///dev/../etc/passwd"})", &error), nullptr);
    EXPECT_EQ(open(R"({"kind":"can"})", &error), nullptr);
    EXPECT_EQ(open(R"({"kind":"can","interface":"gygax-missing0"})", &error), nullptr);
    EXPECT_EQ(open(R"({"kind":"can","interface":"virtual:x","dbc":"garbage"})", &error), nullptr);
    EXPECT_NE(error.find("invalid dbc"), std::string::npos);
    EXPECT_EQ(open(R"({"kind":"modbus","uri":"memory://m-bad-mode","mode":"ascii"})", &error), nullptr);
    EXPECT_EQ(open(R"({"kind":"adsb","uri":"memory://a-bad","format":"beast"})", &error), nullptr);
    EXPECT_EQ(open(R"({"kind":"rosbridge","uri":"wss://x"})", &error), nullptr);
    EXPECT_EQ(open(R"({"kind":"modbus","uri":"memory://m-bad-reg","registers":[{"name":"x","type":"u128"}]})", &error), nullptr);
    EXPECT_TRUE(serialPathAllowed("/dev/ttyUSB0"));
    EXPECT_FALSE(serialPathAllowed("/tmp/x"));
    EXPECT_FALSE(serialPathAllowed("/dev/../etc/x"));
    EXPECT_EQ(deviceKinds().size(), 7U);
}

TEST(CanDevice, DecodesMessagesWithDbcAndJ1939AndTransmitsCommands) {
    auto net = virtualCanNetwork("dev-can");
    auto ecu = net->attach("ecu");
    json::Value config = cfg(R"({"kind":"can","interface":"virtual:dev-can","j1939":true})");
    config["dbc"] = kDbc;
    std::string error;
    auto dev = openDevice(config, &error);
    ASSERT_TRUE(dev) << error;
    EXPECT_EQ(dev->kind(), "can");

    auto db = *bus::DbcDatabase::parse(kDbc);
    ASSERT_EQ(ecu->send(*db.encode("Engine", {{"EngineSpeed", 2500.0}, {"Gear", 3.0}})), 0);
    ASSERT_EQ(ecu->send(bus::CanFrame::make(bus::j1939::Id{3, bus::j1939::kPgnEec1, 0xFF, 0}.encode(),
                                            *bus::j1939::encodeEec1(1800.0, 10.0), true)),
              0);
    ASSERT_TRUE(support::waitUntil([&] { return dev->state().getInt("frames_rx") >= 2; }));
    const auto state = dev->state();
    bool sawDbc = false;
    bool sawJ1939 = false;
    for (const auto& m : state.find("messages")->asArray()) {
        if (m.getString("name") == "Engine") {
            sawDbc = true;
            EXPECT_DOUBLE_EQ(m.find("signals")->find("EngineSpeed")->getDouble("value"), 2500.0);
            EXPECT_EQ(m.find("signals")->find("Gear")->getString("choice"), "D");
            EXPECT_EQ(m.getInt("count"), 1);
        }
        if (m.getBool("extended")) {
            sawJ1939 = true;
            EXPECT_DOUBLE_EQ(m.find("signals")->find("engine_speed")->getDouble("value"), 1800.0);
        }
    }
    EXPECT_TRUE(sawDbc);
    EXPECT_TRUE(sawJ1939);

    auto sent = dev->command(cfg(R"({"command":"send","id":291,"data":"DE AD BE EF"})"));
    ASSERT_EQ(sent.status, 200);
    bus::CanFrame f;
    ASSERT_EQ(ecu->receive(f, kDeviceReceiveTimeout), 0);
    EXPECT_EQ(f.id, 0x123U);
    EXPECT_EQ(f.length, 4);
    EXPECT_EQ(f.data[0], 0xDE);
    auto encoded = dev->command(cfg(R"({"command":"encode","message":"Engine","values":{"EngineSpeed":1000}})"));
    ASSERT_EQ(encoded.status, 200);
    ASSERT_EQ(ecu->receive(f, kDeviceReceiveTimeout), 0);
    EXPECT_EQ(f.id, 500U);
    auto request = dev->command(cfg(R"({"command":"request_pgn","pgn":65262,"destination":0,"source":249})"));
    ASSERT_EQ(request.status, 200);
    ASSERT_EQ(ecu->receive(f, kDeviceReceiveTimeout), 0);
    EXPECT_EQ(f.id, 0x18EA00F9U);

    EXPECT_EQ(dev->command(cfg(R"({"command":"send","id":4096,"data":"00"})")).status, 200);
    EXPECT_EQ(dev->command(cfg(R"({"command":"send","id":99999999999,"data":"00"})")).status, 400);
    EXPECT_EQ(dev->command(cfg(R"({"command":"send","id":1,"data":"0"})")).status, 400);
    EXPECT_EQ(dev->command(cfg(R"({"command":"send","id":1,"data":"0011223344556677AA"})")).status, 400);
    EXPECT_EQ(dev->command(cfg(R"({"command":"encode","message":"Nope"})")).status, 400);
    EXPECT_EQ(dev->command(cfg(R"({"command":"explode"})")).status, 400);
    EXPECT_EQ(dev->state().getInt("frames_tx"), 4);
}

TEST(CanDevice, EncodeNeedsADbcDatabase) {
    auto dev = open(R"({"kind":"can","interface":"virtual:dev-nodbc"})");
    ASSERT_TRUE(dev);
    EXPECT_EQ(dev->command(cfg(R"({"command":"encode","message":"X"})")).status, 409);
}

TEST(ObdDevice, ReadsLiveDataFromASimulatedEcu) {
    auto net = virtualCanNetwork("dev-obd");
    bus::VirtualObdEcu ecu(net->attach("ecu"), 0, [](std::uint8_t pid) -> std::optional<double> {
        if (pid == 0x0C) return 2200.0;
        if (pid == 0x0D) return 64.0;
        if (pid == 0x05) return 88.0;
        return std::nullopt;
    });
    ecu.setVin("1GYGAXOBD00000001");
    ecu.setDtcs({0x0300});
    ecu.start();
    auto dev = open(R"({"kind":"obd","interface":"virtual:dev-obd","poll_ms":100})");
    ASSERT_TRUE(dev);
    ASSERT_TRUE(support::waitUntil([&] { return dev->connected(); }, 5s));
    const auto state = dev->state();
    EXPECT_DOUBLE_EQ(state.find("values")->find("engine_speed")->getDouble("value"), 2200.0);
    EXPECT_DOUBLE_EQ(state.find("values")->find("vehicle_speed")->getDouble("value"), 64.0);
    const auto pid = dev->command(cfg(R"({"command":"read_pid","pid":5})"));
    ASSERT_EQ(pid.status, 200);
    EXPECT_DOUBLE_EQ(pid.body.getDouble("value"), 88.0);
    EXPECT_EQ(dev->command(cfg(R"({"command":"vin"})")).body.getString("vin"), "1GYGAXOBD00000001");
    EXPECT_EQ(dev->command(cfg(R"({"command":"dtcs"})")).body.find("codes")->asArray()[0].asString(), "P0300");
    EXPECT_FALSE(dev->command(cfg(R"({"command":"supported"})")).body.find("pids")->asArray().empty());
    EXPECT_EQ(dev->command(cfg(R"({"command":"read_pid","pid":999})")).status, 400);
    EXPECT_EQ(dev->command(cfg(R"({"command":"read_pid","pid":11})")).status, 504);
    ecu.stop();
}

TEST(ModbusDevice, PollsRegistersScalesValuesAndWrites) {
    auto peer = net::memoryLinkPeer("plc-1");
    bus::modbus::Slave slave(3);
    slave.holding = {{0, 235}, {1, 0xFFFB}, {2, 0x41C8}, {3, 0x0000}, {10, 0}};
    slave.input = {{0, 1000}};
    slave.coils = {{0, true}, {1, false}};
    std::atomic<bool> run{true};
    std::jthread server([&] {
        while (run.load()) slave.serveOne(*peer, bus::modbus::Mode::Tcp, 20ms);
    });
    auto dev = open(R"({"kind":"modbus","uri":"memory://plc-1","mode":"tcp","unit":3,"poll_ms":50,"registers":[
        {"name":"temperature","address":0,"type":"u16","scale":0.1},
        {"name":"delta","address":1,"type":"i16"},
        {"name":"setpoint","address":2,"type":"f32"},
        {"name":"pressure","area":"input","address":0,"scale":0.01,"offset":-1},
        {"name":"pump","area":"coil","address":0}]})");
    ASSERT_TRUE(dev);
    ASSERT_TRUE(support::waitUntil([&] { return dev->connected() && dev->state().find("values")->contains("pump"); }, 3s));
    const auto values = *dev->state().find("values");
    EXPECT_NEAR(values.getDouble("temperature"), 23.5, 1e-9);
    EXPECT_DOUBLE_EQ(values.getDouble("delta"), -5.0);
    EXPECT_NEAR(values.getDouble("setpoint"), 25.0, 1e-4);
    EXPECT_NEAR(values.getDouble("pressure"), 9.0, 1e-9);
    EXPECT_DOUBLE_EQ(values.getDouble("pump"), 1.0);

    ASSERT_EQ(dev->command(cfg(R"({"command":"write_register","address":10,"value":777})")).status, 200);
    EXPECT_EQ(slave.holding[10], 777);
    ASSERT_EQ(dev->command(cfg(R"({"command":"write_registers","address":0,"values":[1,2]})")).status, 200);
    EXPECT_EQ(slave.holding[1], 2);
    ASSERT_EQ(dev->command(cfg(R"({"command":"write_coil","address":1,"value":true})")).status, 200);
    EXPECT_TRUE(slave.coils[1]);
    const auto read = dev->command(cfg(R"({"command":"read","area":"holding","address":0,"count":2})"));
    ASSERT_EQ(read.status, 200);
    EXPECT_EQ(read.body.find("values")->asArray()[0].asInt(), 1);
    const auto missing = dev->command(cfg(R"({"command":"read","address":900})"));
    EXPECT_EQ(missing.status, 502);
    EXPECT_EQ(missing.body.find("error")->getString("code"), "modbus_exception");
    EXPECT_EQ(dev->command(cfg(R"({"command":"write_register","address":1,"value":70000})")).status, 400);
    EXPECT_EQ(dev->command(cfg(R"({"command":"read","area":"flash","address":0})")).status, 400);
    EXPECT_EQ(dev->command(cfg(R"({"command":"read"})")).status, 400);
    dev.reset();
    run = false;
}

TEST(NmeaDevice, TracksAFixFromASerialStyleFeed) {
    auto peer = net::memoryLinkPeer("gps-1");
    auto dev = open(R"({"kind":"nmea","uri":"memory://gps-1"})");
    ASSERT_TRUE(dev);
    EXPECT_FALSE(dev->connected());
    const std::string feed = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n$GPRMC,123519,A,4807.038,N,01131.000,E,"
                             "022.4,084.4,230394,003.1,W*6A\r\n"
                             "$GPGGA,broken*00\r\n";
    (void)peer->write({reinterpret_cast<const std::uint8_t*>(feed.data()), feed.size()});
    ASSERT_TRUE(support::waitUntil([&] { return dev->state().getInt("sentences") >= 2; }));
    const auto state = dev->state();
    EXPECT_TRUE(state.getBool("valid"));
    EXPECT_NEAR(state.getDouble("latitude"), 48.1173, 1e-6);
    EXPECT_DOUBLE_EQ(state.getDouble("speed_knots"), 22.4);
    EXPECT_GE(state.getInt("invalid_sentences"), 1);
    EXPECT_TRUE(dev->connected());
    EXPECT_EQ(dev->command(cfg(R"({"command":"x"})")).status, 400);
    EXPECT_TRUE(dev->commands().empty());
}

TEST(AdsbDevice, TracksAircraftFromAvrAndSbsFeeds) {
    auto avrPeer = net::memoryLinkPeer("adsb-avr");
    auto avr = open(R"({"kind":"adsb","uri":"memory://adsb-avr","format":"avr"})");
    ASSERT_TRUE(avr);
    const std::string frames = "*8D4840D6202CC371C32CE0576098;\n*8D40621D58C382D690C8AC2863A7;\n*8D40621D58C386435CC412692AD6;\n";
    (void)avrPeer->write({reinterpret_cast<const std::uint8_t*>(frames.data()), frames.size()});
    ASSERT_TRUE(support::waitUntil([&] { return avr->state().find("aircraft")->asArray().size() == 2; }));
    bool klm = false;
    const auto tracked = avr->state();
    for (const auto& a : tracked.find("aircraft")->asArray()) klm = klm || a.getString("callsign") == "KLM1023";
    EXPECT_TRUE(klm);
    EXPECT_TRUE(avr->connected());

    auto sbsPeer = net::memoryLinkPeer("adsb-sbs");
    auto sbs = open(R"({"kind":"adsb","uri":"memory://adsb-sbs","format":"sbs","reference":{"latitude":47,"longitude":8}})");
    ASSERT_TRUE(sbs);
    const std::string line = "MSG,3,1,1,ABCDEF,1,d,t,d,t,,35000,,,47.5,8.5,,,0,0,0,0\r\n";
    (void)sbsPeer->write({reinterpret_cast<const std::uint8_t*>(line.data()), line.size()});
    ASSERT_TRUE(support::waitUntil([&] { return !sbs->state().find("aircraft")->asArray().empty(); }));
    EXPECT_EQ(sbs->state().find("aircraft")->asArray()[0].getString("icao"), "ABCDEF");
}

TEST(MavlinkDevice, ExposesTelemetryAndGuardsCommands) {
    auto peer = net::memoryLinkPeer("drone-1");
    testing_support::FakeAutopilot autopilot(peer);
    auto dev = open(R"({"kind":"mavlink","uri":"memory://drone-1"})");
    ASSERT_TRUE(dev);
    ASSERT_TRUE(support::waitUntil([&] { return dev->connected(); }, 3s));
    ASSERT_TRUE(support::waitUntil([&] { return dev->state().getInt("messages") >= 6; }));
    const auto s = dev->state();
    EXPECT_NEAR(s.getDouble("latitude"), 47.397742, 1e-6);
    EXPECT_EQ(s.find("battery")->getInt("remaining_percent"), 87);

    EXPECT_EQ(dev->command(cfg(R"({"command":"arm"})")).status, 200);
    ASSERT_TRUE(support::waitUntil([&] { return dev->state().getBool("armed"); }));
    EXPECT_EQ(dev->command(cfg(R"({"command":"set_mode","mode":"GUIDED"})")).status, 200);
    EXPECT_EQ(autopilot.mode.load(), 4U);
    EXPECT_EQ(dev->command(cfg(R"({"command":"set_mode","mode":"WARP"})")).status, 400);
    EXPECT_EQ(dev->command(cfg(R"({"command":"takeoff","altitude":15})")).status, 200);
    EXPECT_EQ(dev->command(cfg(R"({"command":"takeoff","altitude":5000})")).status, 400);
    EXPECT_EQ(dev->command(cfg(R"({"command":"takeoff"})")).status, 400);
    EXPECT_EQ(dev->command(cfg(R"({"command":"goto","latitude":47.4,"longitude":8.5,"altitude":30})")).status, 200);
    EXPECT_EQ(dev->command(cfg(R"({"command":"goto","latitude":47.4})")).status, 400);
    EXPECT_EQ(dev->command(cfg(R"({"command":"goto","latitude":95,"longitude":8})")).status, 400);
    const auto param = dev->command(cfg(R"({"command":"param_get","name":"BATT_CAPACITY"})"));
    ASSERT_EQ(param.status, 200);
    EXPECT_DOUBLE_EQ(param.body.getDouble("value"), 3300.0);
    EXPECT_EQ(dev->command(cfg(R"({"command":"param_set","name":"WPNAV_SPEED","value":700})")).status, 200);
    EXPECT_EQ(dev->command(cfg(R"({"command":"land"})")).status, 200);
    EXPECT_EQ(dev->command(cfg(R"({"command":"rtl"})")).status, 200);
    EXPECT_EQ(dev->command(cfg(R"({"command":"request_rate","message_id":33,"hz":4})")).status, 200);
    EXPECT_EQ(dev->command(cfg(R"({"command":"command_long","id":400,"params":[0]})")).status, 200);
    autopilot.rejectArm = true;
    const auto rejected = dev->command(cfg(R"({"command":"command_long","id":400,"params":[1]})"));
    EXPECT_EQ(rejected.status, 200);
    EXPECT_EQ(rejected.body.getInt("result"), 4);
    EXPECT_EQ(dev->command(cfg(R"({"command":"jetpack"})")).status, 400);
}

TEST(RosDevice, TalksToARosbridgeOverRealTcpAndEnforcesTwistLimits) {
    TcpRosServer server;
    const auto uri = std::format("ws://127.0.0.1:{}/", server.port);
    json::Value config = cfg(R"({"kind":"rosbridge","subscribe":[{"topic":"/odom","type":"nav_msgs/Odometry"}]})");
    config["uri"] = uri;
    std::string error;
    auto dev = openDevice(config, &error);
    ASSERT_TRUE(dev) << error;
    ASSERT_TRUE(support::waitUntil(
        [&] { return dev->state().find("topics")->find("/odom") != nullptr && dev->state().find("topics")->find("/odom")->isObject(); }));
    EXPECT_DOUBLE_EQ(dev->state().find("topics")->find("/odom")->getDouble("x"), 1.5);
    EXPECT_EQ(dev->command(cfg(R"({"command":"twist","linear":0.4,"angular":-0.2})")).status, 200);
    EXPECT_EQ(dev->command(cfg(R"({"command":"twist","linear":50,"angular":0})")).status, 400);
    EXPECT_EQ(dev->command(cfg(R"({"command":"publish","topic":"/chatter","type":"std_msgs/String","msg":{"data":"hi"}})")).status, 200);
    const auto call = dev->command(cfg(R"({"command":"call_service","service":"/rosapi/nodes"})"));
    ASSERT_EQ(call.status, 200);
    EXPECT_EQ(call.body.find("values")->find("nodes")->asArray()[0].asString(), "/robot_state_publisher");
    EXPECT_EQ(dev->command(cfg(R"({"command":"subscribe","topic":"/scan"})")).status, 200);
    EXPECT_EQ(dev->command(cfg(R"({"command":"unsubscribe","topic":"/scan"})")).status, 200);
    ASSERT_TRUE(support::waitUntil([&] {
        std::lock_guard lock(server.mutex);
        return server.received.size() >= 7;
    }));
    std::lock_guard lock(server.mutex);
    bool sawTwist = false;
    for (const auto& m : server.received) {
        if (m.getString("op") == "publish" && m.getString("topic") == "/cmd_vel") {
            sawTwist = true;
            EXPECT_DOUBLE_EQ(m.find("msg")->find("linear")->getDouble("x"), 0.4);
        }
    }
    EXPECT_TRUE(sawTwist);
}

TEST(DeviceRegistry, ValidatesIdsAndManagesLifetimes) {
    DeviceRegistry reg;
    std::string error;
    auto peerA = net::memoryLinkPeer("reg-a");
    ASSERT_TRUE(reg.add("gps.main-1", open(R"({"kind":"nmea","uri":"memory://reg-a"})"), &error)) << error;
    EXPECT_FALSE(reg.add("gps.main-1", open(R"({"kind":"nmea","uri":"memory://reg-b"})"), &error));
    EXPECT_NE(error.find("exists"), std::string::npos);
    EXPECT_FALSE(reg.add("bad id!", open(R"({"kind":"nmea","uri":"memory://reg-c"})"), &error));
    EXPECT_FALSE(reg.add("", open(R"({"kind":"nmea","uri":"memory://reg-d"})"), &error));
    EXPECT_FALSE(reg.add(std::string(65, 'x'), open(R"({"kind":"nmea","uri":"memory://reg-e"})"), &error));
    EXPECT_EQ(reg.ids(), std::vector<std::string>{"gps.main-1"});
    EXPECT_EQ(reg.list().asArray()[0].getString("kind"), "nmea");
    EXPECT_NE(reg.get("gps.main-1"), nullptr);
    EXPECT_EQ(reg.get("other"), nullptr);
    EXPECT_TRUE(reg.remove("gps.main-1"));
    EXPECT_FALSE(reg.remove("gps.main-1"));
    reg.clear();
    EXPECT_TRUE(reg.ids().empty());
}

namespace {

class DeviceServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        log::setLevel(log::Level::Error);
        service::ServiceConfig c;
        c.port = 0;
        c.token = "tok";
        c.engines = {"echo"};
        c.enableDeviceCommands = commands();
        svc = std::make_unique<service::Service>(c);
        std::string err;
        ASSERT_TRUE(svc->start(&err)) << err;
        base = "http://127.0.0.1:" + std::to_string(svc->port());
    }

    virtual bool commands() const { return false; }

    net::ClientResponse call(const std::string& method, const std::string& path, const std::string& body = {}) {
        return net::httpRequest(*net::Url::parse(base + path), method, body,
                                {{"Authorization", "Bearer tok"}, {"Content-Type", "application/json"}});
    }

    std::unique_ptr<service::Service> svc;
    std::string base;
};

class DeviceServiceWithCommands : public DeviceServiceTest {
    bool commands() const override { return true; }
};

} // namespace

TEST_F(DeviceServiceTest, ManagesDevicesAndKeepsCommandsDisabledByDefault) {
    auto listed = *json::parse(call("GET", "/v1/devices").body);
    EXPECT_FALSE(listed.getBool("commands_enabled"));
    EXPECT_EQ(listed.find("kinds")->asArray().size(), 7U);

    auto peer = net::memoryLinkPeer("svc-gps");
    const auto added = call("POST", "/v1/devices", R"({"id":"gps","kind":"nmea","uri":"memory://svc-gps"})");
    ASSERT_EQ(added.status, 201) << added.body;
    EXPECT_EQ(call("POST", "/v1/devices", R"({"id":"gps","kind":"nmea","uri":"memory://svc-gps2"})").status, 409);
    EXPECT_EQ(call("POST", "/v1/devices", R"({"id":"x","kind":"nope"})").status, 400);

    const std::string feed = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    (void)peer->write({reinterpret_cast<const std::uint8_t*>(feed.data()), feed.size()});
    ASSERT_TRUE(support::waitUntil([&] {
        auto state = json::parse(call("GET", "/v1/devices/gps/state").body);
        return state && state->find("state")->getBool("valid");
    }));
    const auto state = *json::parse(call("GET", "/v1/devices/gps/state").body);
    EXPECT_TRUE(state.getBool("connected"));
    EXPECT_NEAR(state.find("state")->getDouble("latitude"), 48.1173, 1e-6);

    const auto denied = call("POST", "/v1/devices/gps/command", R"({"command":"anything"})");
    EXPECT_EQ(denied.status, 403);
    EXPECT_NE(denied.body.find("commands_disabled"), std::string::npos);
    EXPECT_EQ(call("GET", "/v1/devices/missing/state").status, 404);
    EXPECT_EQ(call("DELETE", "/v1/devices/missing").status, 404);

    const auto metrics = call("GET", "/metrics").body;
    EXPECT_NE(metrics.find("gygax_devices 1"), std::string::npos);
    EXPECT_NE(metrics.find("gygax_device_connected{device=\"gps\",kind=\"nmea\"} 1"), std::string::npos);

    const auto viaTool = call("POST", "/v1/tools/device.state/invoke", R"({"input":"gps"})");
    ASSERT_EQ(viaTool.status, 200);
    EXPECT_NE(viaTool.body.find("latitude"), std::string::npos);
    EXPECT_EQ(call("POST", "/v1/tools/device.command/invoke", R"({"input":"{}"})").status, 404);
    EXPECT_EQ(call("POST", "/v1/tools/device.state/invoke", R"({"input":"ghost"})").status, 422);
    const auto agent =
        call("POST", "/v1/chat/completions", R"({"model":"gygax-agent:echo","messages":[{"role":"user","content":"!tool device.list "}]})");
    EXPECT_NE(agent.body.find("gps"), std::string::npos);

    EXPECT_EQ(call("DELETE", "/v1/devices/gps").status, 200);
    EXPECT_EQ(call("GET", "/v1/devices/gps/state").status, 404);
}

TEST_F(DeviceServiceWithCommands, RoutesCommandsToDevicesAndToolsWhenEnabled) {
    auto peer = net::memoryLinkPeer("svc-drone");
    testing_support::FakeAutopilot autopilot(peer);
    ASSERT_EQ(call("POST", "/v1/devices", R"({"id":"drone","kind":"mavlink","uri":"memory://svc-drone"})").status, 201);
    ASSERT_TRUE(support::waitUntil(
        [&] {
            auto state = json::parse(call("GET", "/v1/devices/drone/state").body);
            return state && state->getBool("connected");
        },
        4s));
    EXPECT_TRUE(json::parse(call("GET", "/v1/devices").body)->getBool("commands_enabled"));

    const auto arm = call("POST", "/v1/devices/drone/command", R"({"command":"arm"})");
    EXPECT_EQ(arm.status, 200) << arm.body;
    EXPECT_TRUE(autopilot.armed.load());
    const auto setMode = call("POST", "/v1/devices/drone/command", R"({"command":"command_long","id":176,"params":[1,5]})");
    EXPECT_EQ(setMode.status, 200);
    EXPECT_EQ(autopilot.mode.load(), 5U);
    EXPECT_EQ(call("POST", "/v1/devices/drone/command", R"({"command":"levitate"})").status, 400);
    EXPECT_EQ(call("POST", "/v1/devices/ghost/command", R"({"command":"arm"})").status, 404);

    const auto tool = call("POST", "/v1/tools/device.command/invoke", R"({"input":{"device":"drone","command":"disarm"}})");
    EXPECT_EQ(tool.status, 200) << tool.body;
    EXPECT_FALSE(autopilot.armed.load());
    EXPECT_EQ(call("POST", "/v1/tools/device.command/invoke", R"({"input":{"device":"drone","command":"takeoff","altitude":9999}})").status,
              422);

    const auto rpc = call("POST", "/rpc", R"({"jsonrpc":"2.0","id":1,"method":"devices.state","params":{"device":"drone"}})");
    EXPECT_NE(rpc.body.find("\"kind\":\"mavlink\""), std::string::npos);
}
