#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <span>

#include <gygax/net/link.hpp>
#include <gygax/robotics/mavlink.hpp>

#include "fake_autopilot.hpp"
#include "support.hpp"

using namespace gygax;
using namespace gygax::robotics::mavlink;
using gygax::testing_support::FakeAutopilot;
using namespace std::chrono_literals;

namespace {

struct Golden {
    const char* name;
    int version;
    const char* hex;
    const char* fields;
};

const Golden kGolden[] = {
    {"heartbeat", 2, "fd090000070101000000040000000203510403381c",
     R"J({"msg": "HEARTBEAT", "sysid": 1, "comp": 1, "seq": 7, "type": 2, "autopilot": 3, "base_mode": 81, "custom_mode": 4, "system_status": 4})J"},
    {"attitude", 2, "fd1c00000801011e000040e20100cdcccc3dcdcc4cbe000040400ad7233c0ad7a33c8fc2f53c300d",
     R"J({"msg": "ATTITUDE", "sysid": 1, "comp": 1, "seq": 8, "time_boot_ms": 123456, "roll": 0.1, "pitch": -0.2, "yaw": 3.0, "rollspeed": 0.01, "pitchspeed": 0.02, "yawspeed": 0.03})J"},
    {"global_position", 2, "fd1c0000090101210000881300004c52401c80e3140570640800e02e0000640038ff1e0078695d05",
     R"J({"msg": "GLOBAL_POSITION_INT", "sysid": 1, "comp": 1, "seq": 9, "time_boot_ms": 5000, "lat": 473977420, "lon": 85255040, "alt": 550000, "relative_alt": 12000, "vx": 100, "vy": -200, "vz": 30, "hdg": 27000})J"},
    {"arm", 2, "fd2000000affbe4c00000000803f00000000000000000000000000000000000000000000000090010101109c",
     R"J({"msg": "COMMAND_LONG", "sysid": 255, "comp": 190, "seq": 10, "target_system": 1, "target_component": 1, "command": 400, "confirmation": 0, "param1": 1.0, "param2": 0.0})J"},
    {"statustext", 2, "fd1c00000b0101fd00000450726541726d3a20436f6d70617373206e6f74206865616c7468790675",
     R"J({"msg": "STATUSTEXT", "sysid": 1, "comp": 1, "seq": 11, "severity": 4, "text": "PreArm: Compass not healthy"})J"},
    {"gps_raw", 2, "fd1e00000c010118000000401e18240a06004c52401c80e31405706408007800c80064007869030c605f",
     R"J({"msg": "GPS_RAW_INT", "sysid": 1, "comp": 1, "seq": 12, "fix_type": 3, "lat": 473977420, "lon": 85255040, "alt": 550000, "eph": 120, "epv": 200, "vel": 100, "cog": 27000, "satellites_visible": 12, "time_usec": 1700000000000000})J"},
    {"v1_heartbeat", 1, "fe09050101000000000001035903038e35",
     R"J({"msg": "HEARTBEAT", "sysid": 1, "comp": 1, "seq": 5, "type": 1, "autopilot": 3, "base_mode": 89, "custom_mode": 0, "system_status": 3})J"},
    {"param_value", 2, "fd1900000d010116000000404e45bc020c00424154545f43415041434954590000000970b6",
     R"J({"msg": "PARAM_VALUE", "sysid": 1, "comp": 1, "seq": 13, "param_id": "BATT_CAPACITY", "param_value": 3300.0, "param_type": 9, "param_count": 700, "param_index": 12})J"},
    {"sys_status", 2, "fd1f00000e01010100000300000003000000030000005e01442f6aff000000000000000000000000574557",
     R"J({"msg": "SYS_STATUS", "sysid": 1, "comp": 1, "seq": 14, "load": 350, "voltage_battery": 12100, "current_battery": -150, "battery_remaining": 87})J"},
};

std::vector<std::uint8_t> fromHex(const std::string& hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

} // namespace

TEST(MavlinkTable, CoversTheCommonDialect) {
    EXPECT_GE(messageCount(), 200U);
    const auto* hb = findMessage(0);
    ASSERT_NE(hb, nullptr);
    EXPECT_STREQ(hb->name, "HEARTBEAT");
    EXPECT_EQ(hb->crcExtra, 50);
    EXPECT_EQ(findMessage("COMMAND_LONG")->id, 76U);
    EXPECT_EQ(findMessage("COMMAND_LONG")->crcExtra, 152);
    EXPECT_EQ(findMessage(std::string_view("NOT_A_MESSAGE")), nullptr);
    EXPECT_EQ(findMessage(60000U), nullptr);
}

TEST(MavlinkCodec, ParsesFramesProducedByTheReferenceImplementation) {
    for (const auto& g : kGolden) {
        Parser parser;
        const auto frames = parser.feed(fromHex(g.hex));
        ASSERT_EQ(frames.size(), 1U) << g.name;
        const auto& f = frames[0];
        const auto expected = *json::parse(g.fields);
        const auto* def = findMessage(f.messageId);
        ASSERT_NE(def, nullptr) << g.name;
        EXPECT_EQ(def->name, expected.getString("msg")) << g.name;
        EXPECT_EQ(f.version, g.version) << g.name;
        EXPECT_EQ(f.systemId, expected.getInt("sysid")) << g.name;
        EXPECT_EQ(f.componentId, expected.getInt("comp")) << g.name;
        EXPECT_EQ(f.sequence, expected.getInt("seq")) << g.name;
        const auto decoded = decodePayload(*def, f.payload);
        for (const auto& [key, value] : expected.asObject()) {
            if (key == "msg" || key == "sysid" || key == "comp" || key == "seq") continue;
            if (value.isString())
                EXPECT_EQ(decoded.getString(key), value.asString()) << g.name << "." << key;
            else
                EXPECT_NEAR(decoded.getDouble(key), value.asDouble(), std::fabs(value.asDouble()) * 1e-6 + 1e-6) << g.name << "." << key;
        }
        EXPECT_EQ(parser.stats().crcErrors, 0U);
    }
}

TEST(MavlinkCodec, SerializesBytesIdenticalToTheReferenceImplementation) {
    for (const auto& g : kGolden) {
        Parser parser;
        const auto frames = parser.feed(fromHex(g.hex));
        ASSERT_EQ(frames.size(), 1U) << g.name;
        const auto* def = findMessage(frames[0].messageId);
        const auto decoded = decodePayload(*def, frames[0].payload);
        std::string error;
        const auto payload = encodePayload(*def, decoded, &error);
        ASSERT_TRUE(payload) << g.name << ": " << error;
        Frame f = frames[0];
        f.payload = *payload;
        std::string hex;
        for (const auto b : serialize(f)) hex += std::format("{:02x}", b);
        EXPECT_EQ(hex, g.hex) << g.name;
    }
}

TEST(MavlinkCodec, EncodesTheDocumentedFieldsOfCommonMessages) {
    const auto* def = findMessage("HEARTBEAT");
    const auto payload = encodePayload(
        *def, *json::parse(R"({"type":2,"autopilot":3,"base_mode":81,"custom_mode":4,"system_status":4,"mavlink_version":3})"));
    ASSERT_TRUE(payload);
    Frame f;
    f.sequence = 7;
    f.systemId = 1;
    f.componentId = 1;
    f.messageId = 0;
    f.payload = *payload;
    std::string hex;
    for (const auto b : serialize(f)) hex += std::format("{:02x}", b);
    EXPECT_EQ(hex, kGolden[0].hex);
}

TEST(MavlinkCodec, ResynchronizesAfterGarbageAndCorruption) {
    Parser parser;
    std::vector<std::uint8_t> stream = {0x00, 0x11, 0xFD, 0x01, 0x02};
    const auto good = fromHex(kGolden[0].hex);
    stream.insert(stream.end(), good.begin(), good.end());
    auto corrupted = fromHex(kGolden[1].hex);
    corrupted[12] ^= 0x40;
    stream.insert(stream.end(), corrupted.begin(), corrupted.end());
    const auto tail = fromHex(kGolden[2].hex);
    stream.insert(stream.end(), tail.begin(), tail.end());
    std::vector<Frame> all;
    for (const auto b : stream) {
        const auto frames = parser.feed(std::span<const std::uint8_t>(&b, 1));
        all.insert(all.end(), frames.begin(), frames.end());
    }
    ASSERT_GE(all.size(), 2U);
    EXPECT_EQ(all.front().messageId, 0U);
    EXPECT_EQ(all.back().messageId, 33U);
    EXPECT_GE(parser.stats().crcErrors, 1U);
    EXPECT_GT(parser.stats().resyncs, 0U);
}

TEST(MavlinkCodec, SurvivesRandomNoiseAndUnknownMessages) {
    std::mt19937 rng(12345);
    Parser parser;
    for (int i = 0; i < 400; ++i) {
        std::vector<std::uint8_t> noise(static_cast<std::size_t>(rng() % 300));
        for (auto& b : noise) b = static_cast<std::uint8_t>(rng());
        if (i % 3 == 0 && !noise.empty()) noise[0] = 0xFD;
        (void)parser.feed(noise);
    }
    Frame unknown;
    unknown.messageId = 65000;
    unknown.payload = {1, 2, 3};
    (void)parser.feed(serialize(unknown));
    const auto frames = parser.feed(fromHex(kGolden[0].hex));
    EXPECT_EQ(frames.size(), 1U);
    EXPECT_GE(parser.stats().unknownMessages, 1U);
}

TEST(MavlinkCodec, EncodeValidatesFields) {
    const auto* def = findMessage("COMMAND_LONG");
    std::string error;
    EXPECT_FALSE(encodePayload(*def, *json::parse(R"({"bogus":1})"), &error));
    EXPECT_NE(error.find("no field"), std::string::npos);
    EXPECT_FALSE(encodePayload(*def, *json::parse(R"({"command":70000})"), &error));
    EXPECT_NE(error.find("out of range"), std::string::npos);
    EXPECT_FALSE(encodePayload(*def, *json::parse(R"({"command":"arm"})"), &error));
    EXPECT_FALSE(encodePayload(*findMessage("STATUSTEXT"),
                               *json::parse(R"({"text":"this text is far too long to fit in fifty characters ok"})"), &error));
    EXPECT_FALSE(encodePayload(*def, *json::parse("[]"), &error));
    EXPECT_TRUE(encodePayload(*def, *json::parse(R"({"command":400,"param1":1})"), &error));
}

TEST(MavlinkCodec, V2TruncatesTrailingZerosAndKeepsOneByte) {
    Frame f;
    f.messageId = 76;
    f.payload.assign(33, 0);
    EXPECT_EQ(serialize(f).size(), 10U + 1U + 2U);
    f.payload[5] = 9;
    EXPECT_EQ(serialize(f).size(), 10U + 6U + 2U);
    EXPECT_EQ(serialize(f, false).size(), 10U + 33U + 2U);
    Parser parser;
    const auto frames = parser.feed(serialize(f));
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].payload.size(), 33U);
    EXPECT_EQ(frames[0].payload[5], 9);
}

TEST(MavlinkVehicle, TracksTelemetryFromAnAutopilot) {
    auto [gcs, drone] = net::makeLinkPair();
    FakeAutopilot autopilot(drone);
    Vehicle vehicle(gcs);
    vehicle.start();
    ASSERT_TRUE(vehicle.waitForHeartbeat(2s));
    ASSERT_TRUE(support::waitUntil([&] { return vehicle.state().messages >= 6; }));
    const auto s = vehicle.state();
    EXPECT_TRUE(s.connected);
    EXPECT_EQ(s.systemId, 1);
    EXPECT_EQ(s.vehicleType, 2);
    EXPECT_FALSE(s.armed);
    EXPECT_NEAR(s.latitude, 47.397742, 1e-6);
    EXPECT_NEAR(s.longitude, 8.525504, 1e-6);
    EXPECT_NEAR(s.altitudeRelative, 12.0, 1e-9);
    EXPECT_NEAR(s.vx, 1.0, 1e-9);
    EXPECT_NEAR(s.vy, -2.0, 1e-9);
    EXPECT_NEAR(s.headingDeg, 270.0, 1e-9);
    EXPECT_NEAR(s.roll, 0.1, 1e-6);
    EXPECT_NEAR(s.batteryVoltage, 12.1, 1e-9);
    EXPECT_NEAR(s.batteryCurrent, 15.0, 1e-9);
    EXPECT_EQ(s.batteryRemaining, 87);
    EXPECT_NEAR(s.groundspeed, 5.5, 1e-6);
    EXPECT_EQ(s.gpsFix, 3);
    EXPECT_EQ(s.satellites, 14);
    const auto json = s.toJson();
    EXPECT_TRUE(json.getBool("connected"));
    EXPECT_NEAR(json.find("battery")->getDouble("voltage"), 12.1, 1e-9);
    EXPECT_TRUE(vehicle.lastMessage("ATTITUDE").has_value());
    EXPECT_FALSE(vehicle.lastMessage("MISSION_CURRENT").has_value());
    EXPECT_EQ(vehicle.parserStats().crcErrors, 0U);
}

TEST(MavlinkVehicle, CommandsAreAcknowledgedAndRejectionsSurface) {
    auto [gcs, drone] = net::makeLinkPair();
    FakeAutopilot autopilot(drone);
    Vehicle vehicle(gcs);
    vehicle.start();
    ASSERT_TRUE(vehicle.waitForHeartbeat(2s));
    ASSERT_EQ(vehicle.arm(), 0);
    ASSERT_TRUE(support::waitUntil([&] { return vehicle.state().armed; }));
    ASSERT_EQ(vehicle.setMode(Vehicle::arducopterMode("GUIDED").value()), 0);
    EXPECT_EQ(autopilot.mode.load(), 4U);
    EXPECT_EQ(vehicle.takeoff(10.0), 0);
    EXPECT_EQ(vehicle.land(), 0);
    EXPECT_EQ(vehicle.returnToLaunch(), 0);
    EXPECT_EQ(vehicle.requestMessageRate(33, 5.0), 0);
    ASSERT_EQ(vehicle.disarm(), 0);
    ASSERT_TRUE(support::waitUntil([&] { return !vehicle.state().armed; }));

    autopilot.rejectArm = true;
    std::uint8_t result = 0;
    EXPECT_EQ(vehicle.sendCommand(kCmdComponentArmDisarm, {1, 0, 0, 0, 0, 0, 0}, &result), -EPERM);
    EXPECT_EQ(result, 4);
    EXPECT_FALSE(Vehicle::arducopterMode("WARP"));
}

TEST(MavlinkVehicle, CommandsTimeOutWhenNothingAnswers) {
    auto [gcs, drone] = net::makeLinkPair();
    FakeAutopilot autopilot(drone);
    autopilot.silentCommands = true;
    VehicleOptions opt;
    opt.commandTimeout = 150ms;
    Vehicle vehicle(gcs, opt);
    vehicle.start();
    ASSERT_TRUE(vehicle.waitForHeartbeat(2s));
    EXPECT_EQ(vehicle.arm(), -ETIMEDOUT);
    EXPECT_GE(autopilot.commands.load(), 1);
}

TEST(MavlinkVehicle, GotoParametersAndValidation) {
    auto [gcs, drone] = net::makeLinkPair();
    FakeAutopilot autopilot(drone);
    Vehicle vehicle(gcs);
    vehicle.start();
    ASSERT_TRUE(vehicle.waitForHeartbeat(2s));
    ASSERT_EQ(vehicle.gotoGlobal(47.4, 8.5, 25.0), 0);
    ASSERT_TRUE(support::waitUntil([&] {
        std::lock_guard lock(autopilot.mutex);
        return autopilot.lastTarget.isObject() && autopilot.lastTarget.getInt("lat_int") != 0;
    }));
    {
        std::lock_guard lock(autopilot.mutex);
        EXPECT_EQ(autopilot.lastTarget.getInt("lat_int"), 474000000);
        EXPECT_EQ(autopilot.lastTarget.getInt("lon_int"), 85000000);
        EXPECT_NEAR(autopilot.lastTarget.getDouble("alt"), 25.0, 1e-6);
        EXPECT_EQ(autopilot.lastTarget.getInt("coordinate_frame"), 6);
    }
    EXPECT_EQ(vehicle.gotoGlobal(91.0, 0.0, 10.0), -EINVAL);
    EXPECT_EQ(vehicle.gotoGlobal(0.0, 181.0, 10.0), -EINVAL);

    double value = 0.0;
    ASSERT_EQ(vehicle.readParameter("BATT_CAPACITY", value), 0);
    EXPECT_DOUBLE_EQ(value, 3300.0);
    ASSERT_EQ(vehicle.setParameter("WPNAV_SPEED", 650.0), 0);
    EXPECT_DOUBLE_EQ(autopilot.params["WPNAV_SPEED"], 650.0);
    EXPECT_EQ(vehicle.readParameter("NO_SUCH_PARAM", value, 100ms), -ETIMEDOUT);
    EXPECT_EQ(vehicle.setParameter("", 1.0), -EINVAL);
    EXPECT_EQ(vehicle.setParameter("THIS_NAME_IS_TOO_LONG", 1.0), -EINVAL);
    EXPECT_EQ(vehicle.sendMessage("NOT_A_MESSAGE", json::Value::object()), -ENOENT);
    EXPECT_EQ(vehicle.sendMessage("COMMAND_LONG", *json::parse(R"({"bogus":1})")), -EINVAL);
}

TEST(MavlinkVehicle, ReportsDisconnectionWhenHeartbeatsStop) {
    auto [gcs, drone] = net::makeLinkPair();
    auto autopilot = std::make_unique<FakeAutopilot>(drone);
    VehicleOptions opt;
    opt.heartbeatTimeout = 300ms;
    Vehicle vehicle(gcs, opt);
    vehicle.start();
    ASSERT_TRUE(vehicle.waitForHeartbeat(2s));
    autopilot.reset();
    EXPECT_TRUE(support::waitUntil([&] { return !vehicle.state().connected; }, 3s));
}

TEST(MavlinkVehicle, MessageHandlerSeesEverythingIncludingStatusText) {
    auto [gcs, drone] = net::makeLinkPair();
    Vehicle vehicle(gcs);
    std::atomic<int> heartbeats{0};
    std::atomic<bool> sawText{false};
    vehicle.onMessage([&](const std::string& name, const Frame&, const json::Value& fields) {
        if (name == "HEARTBEAT") ++heartbeats;
        if (name == "STATUSTEXT" && fields.getString("text") == "EKF3 IMU0 is using GPS") sawText = true;
    });
    vehicle.start();
    Parser ignore;
    const auto* hb = findMessage("HEARTBEAT");
    const auto* st = findMessage("STATUSTEXT");
    Frame f;
    f.systemId = 1;
    f.componentId = 1;
    f.messageId = hb->id;
    f.payload = *encodePayload(*hb, *json::parse(R"({"type":10,"autopilot":3,"base_mode":81})"));
    (void)drone->write(serialize(f));
    f.messageId = st->id;
    f.payload = *encodePayload(*st, *json::parse(R"({"severity":6,"text":"EKF3 IMU0 is using GPS"})"));
    (void)drone->write(serialize(f));
    ASSERT_TRUE(support::waitUntil([&] { return sawText.load(); }));
    EXPECT_GE(heartbeats.load(), 1);
    EXPECT_EQ(vehicle.state().lastStatusText, "EKF3 IMU0 is using GPS");
    EXPECT_EQ(vehicle.state().vehicleType, 10);
}

TEST(ByteLink, MemoryPairsUdpTcpAndUriParsing) {
    auto [a, b] = net::makeLinkPair();
    const std::vector<std::uint8_t> msg = {1, 2, 3};
    ASSERT_EQ(a->write(msg), 0);
    std::vector<std::uint8_t> got;
    ASSERT_EQ(b->read(got, 100ms), 0);
    EXPECT_EQ(got, msg);
    EXPECT_EQ(b->read(got, 10ms), -ETIMEDOUT);

    std::string error;
    auto server = net::openLink("udp://127.0.0.1:47831", &error);
    ASSERT_TRUE(server) << error;
    auto client = net::openLink("udp://127.0.0.1:47831?bind=47832", &error);
    ASSERT_TRUE(client) << error;
    ASSERT_EQ(server->write(msg), -ENOTCONN);
    ASSERT_EQ(client->write(msg), 0);
    ASSERT_EQ(server->read(got, 500ms), 0);
    EXPECT_EQ(got, msg);
    const std::vector<std::uint8_t> reply = {4, 5};
    ASSERT_EQ(server->write(reply), 0);
    ASSERT_EQ(client->read(got, 500ms), 0);
    EXPECT_EQ(got, (std::vector<std::uint8_t>{4, 5}));

    EXPECT_FALSE(net::openLink("tcp://127.0.0.1:1", &error));
    EXPECT_NE(error.find("cannot connect"), std::string::npos);
    EXPECT_FALSE(net::openLink("ftp://x", &error));
    EXPECT_FALSE(net::openLink("udp://nohostport", &error));
    EXPECT_FALSE(net::openLink("serial:///dev/gygax-missing?baud=57600", &error));
    EXPECT_FALSE(net::openLink("serial:///dev/null?baud=abc", &error));
}
