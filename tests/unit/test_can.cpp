#include <gtest/gtest.h>

#include <net/if.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <thread>

#include <gygax/bus/can.hpp>
#include <gygax/bus/canopen.hpp>
#include <gygax/bus/dbc.hpp>
#include <gygax/bus/isotp.hpp>
#include <gygax/bus/j1939.hpp>
#include <gygax/bus/obd2.hpp>

#include "support.hpp"

using namespace gygax::bus;
using namespace std::chrono_literals;

namespace {

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    for (const int v : values) out.push_back(static_cast<std::uint8_t>(v));
    return out;
}

std::vector<std::uint8_t> pattern(std::size_t n) {
    std::vector<std::uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xFF);
    return out;
}

const char* kDbc = R"DBC(VERSION ""

NS_ :

BS_:

BU_: ECU Tester

BO_ 500 Engine: 8 ECU
 SG_ EngineSpeed : 0|16@1+ (0.25,0) [0|16383.75] "rpm" Tester
 SG_ CoolantTemp : 16|8@1+ (1,-40) [-40|215] "degC" Tester
 SG_ Torque : 24|12@1- (0.5,0) [-1024|1023.5] "Nm" Tester
 SG_ Gear : 36|4@1+ (1,0) [0|15] "" Tester

BO_ 768 Chassis: 8 ECU
 SG_ WheelSpeed : 7|16@0+ (0.01,0) [0|655.35] "km/h" Tester
 SG_ SteeringAngle : 23|16@0- (0.1,0) [-3276.8|3276.7] "deg" Tester
 SG_ Mux M : 39|4@0+ (1,0) [0|15] "" Tester
 SG_ MuxA m0 : 47|8@0+ (1,0) [0|255] "" Tester
 SG_ MuxB m1 : 47|8@0+ (2,0) [0|510] "" Tester

BO_ 2147483905 Ext: 8 ECU
 SG_ Volt : 0|32@1+ (0.001,0) [0|4294967.295] "V" Tester

VAL_ 500 Gear 0 "P" 1 "R" 2 "N" 3 "D" ;
)DBC";

std::map<std::string, DecodedSignal> byName(const std::vector<DecodedSignal>& signals) {
    std::map<std::string, DecodedSignal> out;
    for (const auto& s : signals) out[s.name] = s;
    return out;
}

CanFrame frameFromHex(std::uint32_t id, bool extended, const std::string& hex) {
    std::vector<std::uint8_t> data;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) data.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return CanFrame::make(id, data, extended);
}

} // namespace

TEST(CanFrame, DlcMappingCoversClassicAndFdLengths) {
    EXPECT_EQ(dlcToLength(8), 8);
    EXPECT_EQ(dlcToLength(9), 12);
    EXPECT_EQ(dlcToLength(15), 64);
    EXPECT_EQ(lengthToDlc(0), 0);
    EXPECT_EQ(lengthToDlc(8), 8);
    EXPECT_EQ(lengthToDlc(9), 9);
    EXPECT_EQ(lengthToDlc(33), 14);
    EXPECT_EQ(lengthToDlc(64), 15);
}

TEST(CanFrame, MakeClampsAndFormats) {
    const auto std11 = CanFrame::make(0x123, bytes({0xDE, 0xAD}));
    EXPECT_EQ(toString(std11), "123#DEAD");
    EXPECT_FALSE(std11.fd);
    const auto ext = CanFrame::make(0x18F00400, bytes({1}), true);
    EXPECT_EQ(toString(ext), "18F00400#01");
    const auto fd = CanFrame::make(0x10, pattern(20));
    EXPECT_TRUE(fd.fd);
    EXPECT_EQ(fd.length, 20);
    EXPECT_EQ(CanFrame::make(0x10, pattern(200), false, true).length, 64);
    EXPECT_EQ(std11, CanFrame::make(0x123, bytes({0xDE, 0xAD})));
    EXPECT_FALSE(std11 == ext);
}

TEST(LoopbackCan, BroadcastsToOtherNodesButNotBackToTheSender) {
    LoopbackCanNetwork net;
    auto a = net.attach("a");
    auto b = net.attach("b");
    auto c = net.attach("c", true);
    ASSERT_EQ(a->send(CanFrame::make(0x100, bytes({1, 2, 3}))), 0);
    CanFrame f;
    ASSERT_EQ(b->receive(f, 100ms), 0);
    EXPECT_EQ(f.id, 0x100U);
    EXPECT_EQ(f.length, 3);
    EXPECT_GT(f.timestampNs, 0U);
    ASSERT_EQ(c->receive(f, 100ms), 0);
    EXPECT_EQ(a->receive(f, 20ms), -ETIMEDOUT);
    ASSERT_EQ(c->send(CanFrame::make(0x200, bytes({9}))), 0);
    ASSERT_EQ(c->receive(f, 100ms), 0);
    EXPECT_EQ(net.framesCarried(), 2U);
}

TEST(LoopbackCan, FiltersLimitsAndValidation) {
    LoopbackCanNetwork net;
    auto tx = net.attach("tx");
    auto rx = net.attach("rx");
    ASSERT_EQ(rx->setFilters({CanFilter{0x100, 0x7F0, false}}), 0);
    ASSERT_EQ(tx->send(CanFrame::make(0x200, bytes({1}))), 0);
    ASSERT_EQ(tx->send(CanFrame::make(0x105, bytes({2}))), 0);
    ASSERT_EQ(tx->send(CanFrame::make(0x105, bytes({3}), true)), 0);
    CanFrame f;
    ASSERT_EQ(rx->receive(f, 100ms), 0);
    EXPECT_EQ(f.data[0], 2);
    EXPECT_EQ(rx->receive(f, 20ms), -ETIMEDOUT);

    CanFrame bad = CanFrame::make(0x800, bytes({1}));
    EXPECT_EQ(tx->send(bad), -EINVAL);
    CanFrame tooLong = CanFrame::make(0x1, pattern(8));
    tooLong.length = 12;
    EXPECT_EQ(tx->send(tooLong), -EMSGSIZE);

    ASSERT_EQ(rx->setFilters({}), 0);
    for (int i = 0; i < 5000; ++i) ASSERT_EQ(tx->send(CanFrame::make(0x300, bytes({i & 0xFF}))), 0);
    int drained = 0;
    while (rx->receive(f, 1ms) == 0) ++drained;
    EXPECT_EQ(drained, 4096);
}

TEST(SocketCan, ReportsMissingInterfacesAndInvalidNames) {
    std::string error;
    EXPECT_EQ(SocketCanBus::open("gygax-nonexistent0", false, &error), nullptr);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(SocketCanBus::open("", false, &error), nullptr);
    EXPECT_EQ(SocketCanBus::open(std::string(40, 'x'), false, &error), nullptr);
}

TEST(SocketCan, LoopsFramesThroughAVirtualInterfaceWhenPresent) {
    if (!SocketCanBus::supported() || ::if_nametoindex("vcan0") == 0)
        GTEST_SKIP() << "vcan0 is not available; CI creates it with `ip link add dev vcan0 type vcan`";
    std::string error;
    auto tx = SocketCanBus::open("vcan0", true, &error);
    auto rx = SocketCanBus::open("vcan0", true, &error);
    ASSERT_TRUE(tx && rx) << error;
    ASSERT_EQ(rx->setFilters({CanFilter{0x123, 0x7FF, false}}), 0);
    ASSERT_EQ(tx->send(CanFrame::make(0x111, bytes({1}))), 0);
    ASSERT_EQ(tx->send(CanFrame::make(0x123, bytes({0xCA, 0xFE}))), 0);
    CanFrame f;
    ASSERT_EQ(rx->receive(f, 500ms), 0);
    EXPECT_EQ(f.id, 0x123U);
    EXPECT_EQ(f.data[0], 0xCA);
    const auto big = pattern(48);
    ASSERT_EQ(tx->send(CanFrame::make(0x123, big, false, true)), 0);
    ASSERT_EQ(rx->receive(f, 500ms), 0);
    EXPECT_TRUE(f.fd);
    EXPECT_EQ(f.length, 48);
    ASSERT_EQ(tx->send(CanFrame::make(0x18F00400, bytes({7}), true)), 0);
    auto extRx = SocketCanBus::open("vcan0", false, &error);
    ASSERT_TRUE(extRx);
    ASSERT_EQ(tx->send(CanFrame::make(0x18F00400, bytes({7}), true)), 0);
    ASSERT_EQ(extRx->receive(f, 500ms), 0);
    EXPECT_TRUE(f.extended);
    EXPECT_EQ(f.id, 0x18F00400U);
    auto classic = SocketCanBus::open("vcan0", false, &error);
    EXPECT_EQ(classic->send(CanFrame::make(0x1, big, false, true)), -EPROTONOSUPPORT);
}

namespace {

struct IsoTpPair {
    LoopbackCanNetwork net;
    std::shared_ptr<CanBus> busA = net.attach("a");
    std::shared_ptr<CanBus> busB = net.attach("b");
    std::shared_ptr<CanBus> sniffer = net.attach("sniffer");

    IsoTpChannel a(IsoTpOptions o = {}) {
        o.txId = 0x7E0;
        o.rxId = 0x7E8;
        return IsoTpChannel(busA, o);
    }

    IsoTpChannel b(IsoTpOptions o = {}) {
        o.txId = 0x7E8;
        o.rxId = 0x7E0;
        return IsoTpChannel(busB, o);
    }
};

} // namespace

TEST(IsoTp, SingleFramePayloadsArePadded) {
    IsoTpPair p;
    auto a = p.a();
    auto b = p.b();
    ASSERT_EQ(a.send(bytes({0x22, 0xF1, 0x90})), 0);
    std::vector<std::uint8_t> got;
    ASSERT_EQ(b.receive(got, 200ms), 0);
    EXPECT_EQ(got, bytes({0x22, 0xF1, 0x90}));
    CanFrame raw;
    ASSERT_EQ(p.sniffer->receive(raw, 50ms), 0);
    EXPECT_EQ(raw.length, 8);
    EXPECT_EQ(raw.data[0], 0x03);
    EXPECT_EQ(raw.data[7], 0xCC);
}

TEST(IsoTp, MultiFramePayloadsRoundTripWithFlowControl) {
    for (const int blockSizeValue : {0, 1, 4}) {
        const auto blockSize = static_cast<std::uint8_t>(blockSizeValue);
        IsoTpPair p;
        IsoTpOptions rxOpt;
        rxOpt.blockSize = blockSize;
        rxOpt.separationTime = std::chrono::microseconds(200);
        auto sender = p.a();
        auto receiver = p.b(rxOpt);
        const auto payload = pattern(300);
        std::vector<std::uint8_t> got;
        IOResult rc = -1;
        std::thread rx([&] { rc = receiver.receive(got, 3s); });
        ASSERT_EQ(sender.send(payload), 0) << "block size " << static_cast<int>(blockSize);
        rx.join();
        ASSERT_EQ(rc, 0);
        EXPECT_EQ(got, payload);
    }
}

TEST(IsoTp, CanFdCarriesLargeFramesAndEscapedSingleFrames) {
    IsoTpPair p;
    IsoTpOptions fd;
    fd.fd = true;
    auto sender = p.a(fd);
    auto receiver = p.b(fd);
    const auto small = pattern(40);
    ASSERT_EQ(sender.send(small), 0);
    std::vector<std::uint8_t> got;
    ASSERT_EQ(receiver.receive(got, 200ms), 0);
    EXPECT_EQ(got, small);
    CanFrame raw;
    ASSERT_EQ(p.sniffer->receive(raw, 50ms), 0);
    EXPECT_TRUE(raw.fd);
    EXPECT_EQ(raw.data[0], 0x00);
    EXPECT_EQ(raw.data[1], 40);

    const auto large = pattern(1000);
    IOResult rc = -1;
    std::thread rx([&] { rc = receiver.receive(got, 3s); });
    ASSERT_EQ(sender.send(large), 0);
    rx.join();
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(got, large);
}

TEST(IsoTp, ReportsTimeoutsOversizeAndSequenceErrors) {
    IsoTpPair p;
    IsoTpOptions quick;
    quick.timeout = 60ms;
    auto sender = p.a(quick);
    EXPECT_EQ(sender.send(pattern(50)), -ETIMEDOUT);
    EXPECT_EQ(sender.send({}), -EMSGSIZE);
    EXPECT_EQ(sender.send(pattern(5000)), -EMSGSIZE);
    std::vector<std::uint8_t> got;
    EXPECT_EQ(sender.receive(got, 20ms), -ETIMEDOUT);

    auto receiver = p.b(quick);
    IOResult rc = 0;
    std::thread rx([&] { rc = receiver.receive(got, 1s); });
    auto injector = p.net.attach("injector");
    ASSERT_EQ(injector->send(CanFrame::make(0x7E0, bytes({0x10, 0x14, 1, 2, 3, 4, 5, 6}))), 0);
    ASSERT_EQ(injector->send(CanFrame::make(0x7E0, bytes({0x23, 7, 8, 9, 10, 11, 12, 13}))), 0);
    rx.join();
    EXPECT_EQ(rc, -EILSEQ);
}

TEST(IsoTp, WaitFramesAreHonouredAndOverflowAborts) {
    IsoTpPair p;
    IsoTpOptions quick;
    quick.timeout = 200ms;
    auto sender = p.a(quick);
    auto injector = p.net.attach("fc");
    std::thread flow([&] {
        CanFrame f;
        while (p.sniffer->receive(f, 500ms) == 0) {
            if ((f.data[0] >> 4) == 0x1) break;
        }
        (void)injector->send(CanFrame::make(0x7E8, bytes({0x31, 0, 0})));
        (void)injector->send(CanFrame::make(0x7E8, bytes({0x32, 0, 0})));
    });
    EXPECT_EQ(sender.send(pattern(40)), -EMSGSIZE);
    flow.join();
}

TEST(Obd2, ClientReadsLiveDataFromAVirtualEcu) {
    LoopbackCanNetwork net;
    VirtualObdEcu ecu(net.attach("ecu"), 0, [](std::uint8_t pid) -> std::optional<double> {
        switch (pid) {
        case 0x0C: return 3000.0;
        case 0x0D: return 88.0;
        case 0x05: return 92.0;
        case 0x11: return 25.0;
        case 0x2F: return 60.0;
        case 0x42: return 13.8;
        case 0x10: return 12.34;
        default: return std::nullopt;
        }
    });
    ecu.setVin("1GYGAX0TEST123456");
    ecu.setDtcs({0x0133, 0x4300});
    ecu.start();
    Obd2Client client(net.attach("tester"), 0, 500ms);

    const auto rpm = client.readPid(0x0C);
    ASSERT_TRUE(rpm);
    EXPECT_EQ(rpm->name, "engine_speed");
    EXPECT_DOUBLE_EQ(rpm->value, 3000.0);
    EXPECT_EQ(rpm->unit, "rpm");
    EXPECT_DOUBLE_EQ(client.readPid(0x0D)->value, 88.0);
    EXPECT_DOUBLE_EQ(client.readPid(0x05)->value, 92.0);
    EXPECT_NEAR(client.readPid(0x11)->value, 25.0, 0.2);
    EXPECT_NEAR(client.readPid(0x2F)->value, 60.0, 0.2);
    EXPECT_NEAR(client.readPid(0x42)->value, 13.8, 0.001);
    EXPECT_NEAR(client.readPid(0x10)->value, 12.34, 0.01);
    EXPECT_FALSE(client.readPid(0x0B));

    const auto supported = client.supportedPids();
    for (const int pidValue : {0x05, 0x0C, 0x0D, 0x10, 0x11, 0x2F, 0x42}) {
        const auto pid = static_cast<std::uint8_t>(pidValue);
        EXPECT_NE(std::find(supported.begin(), supported.end(), pid), supported.end()) << static_cast<int>(pid);
    }
    EXPECT_EQ(std::find(supported.begin(), supported.end(), 0x0B), supported.end());

    EXPECT_EQ(client.readStoredDtcs(), (std::vector<std::string>{"P0133", "C0300"}));
    EXPECT_EQ(client.readVin(), "1GYGAX0TEST123456");
    EXPECT_GE(ecu.requestsServed(), 10U);
    ecu.stop();
}

TEST(Obd2, TimesOutWithoutAnEcuAndRejectsUnknownModes) {
    LoopbackCanNetwork net;
    Obd2Client lonely(net.attach("tester"), 0, 80ms);
    EXPECT_FALSE(lonely.readPid(0x0C));
    EXPECT_TRUE(lonely.readVin().empty());

    VirtualObdEcu ecu(net.attach("ecu"), 0, [](std::uint8_t) { return std::nullopt; });
    ecu.start();
    Obd2Client client(net.attach("tester2"), 0, 300ms);
    std::vector<std::uint8_t> reply;
    EXPECT_EQ(client.request(0x55, {}, reply), -EREMOTEIO);
    ecu.stop();
}

TEST(Obd2, DecodesStandardFormulas) {
    const std::uint8_t rpm[] = {0x1A, 0xF8};
    EXPECT_DOUBLE_EQ(Obd2Client::decode(0x0C, rpm)->value, 1726.0);
    const std::uint8_t coolant[] = {0x7B};
    EXPECT_DOUBLE_EQ(Obd2Client::decode(0x05, coolant)->value, 83.0);
    const std::uint8_t maf[] = {0x04, 0xD2};
    EXPECT_DOUBLE_EQ(Obd2Client::decode(0x10, maf)->value, 12.34);
    EXPECT_FALSE(Obd2Client::decode(0x0C, std::span<const std::uint8_t>(rpm, 1)));
    EXPECT_FALSE(Obd2Client::decode(0xEE, rpm));
    EXPECT_EQ(Obd2Client::formatDtc(0x01, 0x33), "P0133");
    EXPECT_EQ(Obd2Client::formatDtc(0xC1, 0x00), "U0100");
    EXPECT_EQ(Obd2Client::formatDtc(0x92, 0x01), "B1201");
    EXPECT_STREQ(Obd2Client::pidName(0x0D), "vehicle_speed");
    EXPECT_STREQ(Obd2Client::pidName(0xEE), "unknown");
    EXPECT_EQ(VirtualObdEcu::encode(0x0C, 1726.0).value(), (std::vector<std::uint8_t>{0x1A, 0xF8}));
    EXPECT_FALSE(VirtualObdEcu::encode(0xEE, 1.0));
}

TEST(J1939, IdentifiersRoundTripForBothPduFormats) {
    const auto eec1 = j1939::Id::decode(0x0CF00400);
    EXPECT_EQ(eec1.priority, 3);
    EXPECT_EQ(eec1.pgn, j1939::kPgnEec1);
    EXPECT_EQ(eec1.source, 0);
    EXPECT_TRUE(eec1.isBroadcast());
    EXPECT_EQ((j1939::Id{6, j1939::kPgnEec1, 0xFF, 0}).encode(), 0x18F00400U);

    const auto request = j1939::Id::decode(0x18EA00F9);
    EXPECT_EQ(request.pgn, j1939::kPgnRequest);
    EXPECT_EQ(request.destination, 0x00);
    EXPECT_EQ(request.source, 0xF9);
    EXPECT_EQ((j1939::Id{6, j1939::kPgnRequest, 0x00, 0xF9}).encode(), 0x18EA00F9U);
    const auto frame = j1939::makeRequest(j1939::kPgnEt1, 0x00, 0xF9);
    EXPECT_TRUE(frame.extended);
    EXPECT_EQ(frame.id, 0x18EA00F9U);
    EXPECT_EQ(frame.length, 3);
    EXPECT_EQ(frame.data[0], 0xEE);
    EXPECT_EQ(frame.data[1], 0xFE);
    for (const std::uint32_t id : {0x18FEF100U, 0x0CF00400U, 0x18ECFF00U, 0x1CEBFF17U}) EXPECT_EQ(j1939::Id::decode(id).encode(), id);
}

TEST(J1939, DecodesCommonEngineParameterGroups) {
    const auto eec1 = j1939::encodeEec1(1800.5, 40.0);
    ASSERT_TRUE(eec1);
    std::map<std::string, double> got;
    for (const auto& s : j1939::decode(j1939::kPgnEec1, *eec1)) got[s.name] = s.value;
    EXPECT_DOUBLE_EQ(got["engine_speed"], 1800.5);
    EXPECT_DOUBLE_EQ(got["actual_engine_torque"], 40.0);
    EXPECT_FALSE(j1939::encodeEec1(9000.0, 0.0));

    got.clear();
    for (const auto& s : j1939::decode(j1939::kPgnCcvs, j1939::encodeCcvs(87.5))) got[s.name] = s.value;
    EXPECT_NEAR(got["wheel_based_speed"], 87.5, 1.0 / 256.0);

    const std::uint8_t et1[8] = {130, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const auto sigs = j1939::decode(j1939::kPgnEt1, et1);
    ASSERT_EQ(sigs.size(), 1U);
    EXPECT_DOUBLE_EQ(sigs[0].value, 90.0);

    const std::uint8_t na[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_TRUE(j1939::decode(j1939::kPgnEec1, na).empty());
    EXPECT_TRUE(j1939::decode(12345, na).empty());
    EXPECT_TRUE(j1939::decode(j1939::kPgnEec1, std::span<const std::uint8_t>(na, 3)).empty());
}

TEST(Dbc, ParsesGeometryUnitsAndChoices) {
    std::string error;
    auto db = DbcDatabase::parse(kDbc, &error);
    ASSERT_TRUE(db) << error;
    ASSERT_EQ(db->messages().size(), 3U);
    const auto* engine = db->byName("Engine");
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->id, 500U);
    EXPECT_FALSE(engine->extended);
    EXPECT_EQ(engine->sender, "ECU");
    EXPECT_EQ(engine->signals.size(), 4U);
    const auto* torque = engine->find("Torque");
    ASSERT_NE(torque, nullptr);
    EXPECT_TRUE(torque->isSigned);
    EXPECT_TRUE(torque->littleEndian);
    EXPECT_DOUBLE_EQ(torque->factor, 0.5);
    EXPECT_EQ(engine->find("Gear")->choices.at(3), "D");
    const auto* ext = db->byName("Ext");
    EXPECT_TRUE(ext->extended);
    EXPECT_EQ(ext->id, 257U);
    EXPECT_EQ(db->byId(257, true), ext);
    EXPECT_EQ(db->byId(257, false), nullptr);
    EXPECT_FALSE(db->byName("Chassis")->find("WheelSpeed")->littleEndian);
}

TEST(Dbc, EncodesTheSameBytesAsTheReferenceImplementation) {
    auto db = DbcDatabase::parse(kDbc);
    ASSERT_TRUE(db);
    struct Case {
        const char* message;
        std::map<std::string, double> values;
        const char* hex;
    };
    const Case cases[] = {
        {"Engine", {{"EngineSpeed", 3000.25}, {"CoolantTemp", 90}, {"Torque", -100.5}, {"Gear", 3}}, "e12e82373f000000"},
        {"Engine", {{"EngineSpeed", 0}, {"CoolantTemp", -40}, {"Torque", 1023.5}, {"Gear", 0}}, "000000ff07000000"},
        {"Chassis", {{"WheelSpeed", 123.45}, {"SteeringAngle", -45.6}, {"Mux", 0}, {"MuxA", 200}}, "3039fe3800c80000"},
        {"Chassis", {{"WheelSpeed", 0.01}, {"SteeringAngle", 3276.7}, {"Mux", 1}, {"MuxB", 200}}, "00017fff10640000"},
        {"Ext", {{"Volt", 13.8}}, "e835000000000000"},
    };
    for (const auto& c : cases) {
        std::string error;
        const auto frame = db->encode(c.message, c.values, &error);
        ASSERT_TRUE(frame) << error;
        std::string hex = toString(*frame).substr(toString(*frame).find('#') + 1);
        std::ranges::transform(hex, hex.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
        EXPECT_EQ(hex, c.hex) << c.message;
    }
}

TEST(Dbc, DecodesTheSameValuesAsTheReferenceImplementation) {
    auto db = DbcDatabase::parse(kDbc);
    ASSERT_TRUE(db);
    auto engine = byName(db->decode(frameFromHex(500, false, "e12e82373f000000")));
    EXPECT_DOUBLE_EQ(engine["EngineSpeed"].value, 3000.25);
    EXPECT_DOUBLE_EQ(engine["CoolantTemp"].value, 90.0);
    EXPECT_DOUBLE_EQ(engine["Torque"].value, -100.5);
    EXPECT_EQ(engine["Gear"].choice, "D");
    EXPECT_EQ(engine["EngineSpeed"].unit, "rpm");

    auto raw = byName(db->decode(frameFromHex(500, false, "123456789abcdef0")));
    EXPECT_DOUBLE_EQ(raw["EngineSpeed"].value, 3332.5);
    EXPECT_DOUBLE_EQ(raw["CoolantTemp"].value, 46.0);
    EXPECT_DOUBLE_EQ(raw["Torque"].value, -708.0);
    EXPECT_DOUBLE_EQ(raw["Gear"].value, 9.0);

    auto chassisA = byName(db->decode(frameFromHex(768, false, "3039fe3800c80000")));
    EXPECT_DOUBLE_EQ(chassisA["WheelSpeed"].value, 123.45);
    EXPECT_NEAR(chassisA["SteeringAngle"].value, -45.6, 1e-9);
    EXPECT_DOUBLE_EQ(chassisA["MuxA"].value, 200.0);
    EXPECT_FALSE(chassisA.contains("MuxB"));

    auto chassisB = byName(db->decode(frameFromHex(768, false, "00017fff10640000")));
    EXPECT_DOUBLE_EQ(chassisB["MuxB"].value, 200.0);
    EXPECT_FALSE(chassisB.contains("MuxA"));

    auto ext = byName(db->decode(frameFromHex(257, true, "e835000000000000")));
    EXPECT_NEAR(ext["Volt"].value, 13.8, 1e-9);
    EXPECT_TRUE(db->decode(frameFromHex(0x555, false, "00")).empty());
}

TEST(Dbc, EncodeClampsAndValidates) {
    auto db = DbcDatabase::parse(kDbc);
    ASSERT_TRUE(db);
    std::string error;
    const auto saturated = db->encode("Engine", {{"CoolantTemp", 9999}, {"Torque", -99999}}, &error);
    ASSERT_TRUE(saturated) << error;
    auto decoded = byName(db->decode(*saturated));
    EXPECT_DOUBLE_EQ(decoded["CoolantTemp"].value, 215.0);
    EXPECT_DOUBLE_EQ(decoded["Torque"].value, -1024.0);
    EXPECT_FALSE(db->encode("Nope", {}, &error));
    EXPECT_NE(error.find("unknown message"), std::string::npos);
    EXPECT_FALSE(db->encode("Engine", {{"Bogus", 1}}, &error));
    EXPECT_NE(error.find("no signal"), std::string::npos);
    EXPECT_FALSE(db->encode("Engine", {{"Gear", std::nan("")}}, &error));
}

TEST(Dbc, RejectsMalformedFiles) {
    std::string error;
    EXPECT_FALSE(DbcDatabase::parse("", &error));
    EXPECT_FALSE(DbcDatabase::parse(" SG_ Orphan : 0|8@1+ (1,0) [0|1] \"\" X\n", &error));
    EXPECT_NE(error.find("before any message"), std::string::npos);
    EXPECT_FALSE(DbcDatabase::parse("BO_ 1 M: 8 X\n SG_ S : 0|0@1+ (1,0) [0|1] \"\" X\n", &error));
    EXPECT_FALSE(DbcDatabase::parse("BO_ 1 M: 200 X\n", &error));
}

TEST(Dbc, BitRoundTripsForEveryByteOrderAndSign) {
    for (const bool little : {true, false}) {
        for (const bool isSigned : {false, true}) {
            for (const std::uint32_t length : {1U, 5U, 8U, 12U, 16U, 24U, 32U}) {
                DbcSignal s;
                s.startBit = little ? 3 : 20;
                s.length = length;
                s.littleEndian = little;
                s.isSigned = isSigned;
                const std::int64_t maxV = isSigned ? (1LL << (length - 1)) - 1 : (1LL << length) - 1;
                const std::int64_t minV = isSigned ? -(1LL << (length - 1)) : 0;
                for (const std::int64_t v : {minV, maxV, std::int64_t{0}, maxV / 3}) {
                    std::array<std::uint8_t, 8> data{};
                    DbcDatabase::insertRaw(data, s, v);
                    EXPECT_EQ(DbcDatabase::extractRaw(data, s), v)
                        << "little=" << little << " signed=" << isSigned << " len=" << length << " v=" << v;
                }
            }
        }
    }
}

TEST(CanOpen, NmtHeartbeatAndSyncFrames) {
    const auto nmt = canopen::nmtFrame(canopen::NmtCommand::Start, 5);
    EXPECT_EQ(nmt.id, 0U);
    EXPECT_EQ(nmt.data[0], 0x01);
    EXPECT_EQ(nmt.data[1], 5);
    EXPECT_EQ(canopen::syncFrame().id, 0x80U);
    const auto hb = canopen::parseHeartbeat(CanFrame::make(0x705, bytes({0x05})));
    ASSERT_TRUE(hb);
    EXPECT_EQ(hb->nodeId, 5);
    EXPECT_EQ(hb->state, canopen::NmtState::Operational);
    EXPECT_FALSE(canopen::parseHeartbeat(CanFrame::make(0x705, bytes({0x09}))));
    EXPECT_FALSE(canopen::parseHeartbeat(CanFrame::make(0x700, bytes({0x05}))));
    EXPECT_STREQ(canopen::abortText(0x06020000), "object does not exist in the dictionary");
    EXPECT_STREQ(canopen::abortText(0xDEAD), "unknown abort code");
}

namespace {

class SdoFixture : public ::testing::Test {
protected:
    void SetUp() override {
        dictionary[{0x1000, 0}] = bytes({0x92, 0x01, 0x02, 0x00});
        dictionary[{0x1008, 0}] = std::vector<std::uint8_t>{'G', 'y', 'g', 'a', 'x', ' ', 'd', 'r', 'i', 'v', 'e', ' ', 'v', '1', '.', '2'};
        dictionary[{0x6040, 0}] = bytes({0x0F, 0x00});
        server = std::make_unique<canopen::SdoServer>(
            net.attach("node"), 5,
            [this](std::uint16_t index, std::uint8_t sub) -> std::optional<std::vector<std::uint8_t>> {
                auto it = dictionary.find({index, sub});
                if (it == dictionary.end()) return std::nullopt;
                return it->second;
            },
            [this](std::uint16_t index, std::uint8_t sub, std::span<const std::uint8_t> data) {
                if (index == 0x1000) return false;
                dictionary[{index, sub}].assign(data.begin(), data.end());
                return true;
            });
        running = true;
        thread = std::thread([this] {
            while (running.load()) server->serveOne(20ms);
        });
    }

    void TearDown() override {
        running = false;
        thread.join();
    }

    LoopbackCanNetwork net;
    std::map<std::pair<std::uint16_t, std::uint8_t>, std::vector<std::uint8_t>> dictionary;
    std::unique_ptr<canopen::SdoServer> server;
    std::atomic<bool> running{false};
    std::thread thread;
};

} // namespace

TEST_F(SdoFixture, ExpeditedAndSegmentedTransfers) {
    canopen::SdoClient client(net.attach("master"), 500ms);
    std::vector<std::uint8_t> out;
    ASSERT_EQ(client.read(5, 0x1000, 0, out), 0);
    EXPECT_EQ(out, bytes({0x92, 0x01, 0x02, 0x00}));
    ASSERT_EQ(client.read(5, 0x6040, 0, out), 0);
    EXPECT_EQ(out, bytes({0x0F, 0x00}));
    ASSERT_EQ(client.read(5, 0x1008, 0, out), 0);
    EXPECT_EQ(std::string(out.begin(), out.end()), "Gygax drive v1.2");

    ASSERT_EQ(client.write(5, 0x6040, 0, bytes({0x06, 0x00})), 0);
    EXPECT_EQ((dictionary[{0x6040, 0}]), bytes({0x06, 0x00}));
    const auto longValue = pattern(25);
    ASSERT_EQ(client.write(5, 0x2000, 1, longValue), 0);
    EXPECT_EQ((dictionary[{0x2000, 1}]), longValue);
    ASSERT_EQ(client.read(5, 0x2000, 1, out), 0);
    EXPECT_EQ(out, longValue);
}

TEST_F(SdoFixture, AbortsCarryTheReasonAndTimeoutsAreReported) {
    canopen::SdoClient client(net.attach("master"), 200ms);
    std::vector<std::uint8_t> out;
    EXPECT_EQ(client.read(5, 0x9999, 0, out), -EREMOTEIO);
    EXPECT_EQ(client.lastAbortCode(), 0x06020000U);
    EXPECT_EQ(client.write(5, 0x1000, 0, bytes({1})), -EREMOTEIO);
    EXPECT_EQ(client.lastAbortCode(), 0x06010002U);
    EXPECT_EQ(client.read(9, 0x1000, 0, out), -ETIMEDOUT);
    EXPECT_EQ(client.read(0, 0x1000, 0, out), -EINVAL);
    EXPECT_EQ(client.write(5, 0x1000, 0, {}), -EINVAL);
}
