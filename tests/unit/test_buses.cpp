#include <gtest/gtest.h>

#include <cmath>
#include <thread>

#include <gygax/bus/arinc429.hpp>
#include <gygax/bus/modbus.hpp>
#include <gygax/robotics/adsb.hpp>
#include <gygax/robotics/nmea.hpp>

#include "support.hpp"

using namespace gygax;
using namespace std::chrono_literals;

namespace {

constexpr auto kDefaultModbusClientTimeout = 500ms;

std::vector<std::uint8_t> hexBytes(const std::string& hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

std::string toHex(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    for (const auto b : bytes) out += std::format("{:02x}", b);
    return out;
}

class ModbusRig {
public:
    explicit ModbusRig(bus::modbus::Mode mode) : mode_(mode), slave_(17) {
        auto pair = net::makeLinkPair();
        master = pair.first;
        slaveLink_ = pair.second;
        slave_.holding = {{0, 100}, {1, 200}, {2, 300}, {10, 0xFFFF}, {11, 0x1234}};
        slave_.input = {{0, 7}, {1, 8}, {2, 9}};
        slave_.coils = {{0, true}, {1, false}, {2, true}, {3, false}, {4, false}, {5, false}, {6, false}, {7, false}, {8, true}, {9, true}};
        slave_.discrete = {{0, true}, {1, true}, {2, false}};
        thread_ = std::jthread([this](const std::stop_token& st) {
            while (!st.stop_requested()) slave_.serveOne(*slaveLink_, mode_, 20ms);
        });
    }

    ~ModbusRig() {
        thread_.request_stop();
        thread_.join();
    }

    bus::modbus::Client client(std::chrono::milliseconds timeout = kDefaultModbusClientTimeout) {
        return bus::modbus::Client(master, mode_, timeout);
    }

    std::shared_ptr<net::ByteLink> master;
    bus::modbus::Slave& slave() { return slave_; }

private:
    bus::modbus::Mode mode_;
    bus::modbus::Slave slave_;
    std::shared_ptr<net::ByteLink> slaveLink_;
    std::jthread thread_;
};

} // namespace

TEST(Modbus, CrcAndFramingMatchPublishedExamples) {
    EXPECT_EQ(bus::modbus::crc16(hexBytes("01030000000A")), 0xCDC5);
    EXPECT_EQ(toHex(bus::modbus::frameRtu(1, 3, hexBytes("0000000A"))), "01030000000ac5cd");
    EXPECT_EQ(toHex(bus::modbus::frameRtu(0x11, 3, hexBytes("006b0003"))), "1103006b00037687");
    EXPECT_EQ(toHex(bus::modbus::frameRtu(0x11, 6, hexBytes("00010003"))), "1106000100039a9b");
    EXPECT_EQ(toHex(bus::modbus::frameRtu(0x11, 3, hexBytes("06ae4156524340"))), "110306ae415652434049ad");
    EXPECT_EQ(toHex(bus::modbus::frameTcp(1, 1, 3, hexBytes("0000000a"))), "00010000000601030000000a");
}

TEST(Modbus, ParsingRejectsCorruptionAndTruncation) {
    auto ok = bus::modbus::parseRtu(hexBytes("1103006b00037687"));
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->unit, 0x11);
    EXPECT_EQ(ok->function, 3);
    EXPECT_EQ(ok->data, hexBytes("006b0003"));
    EXPECT_FALSE(bus::modbus::parseRtu(hexBytes("1103006b00037688")));
    EXPECT_FALSE(bus::modbus::parseRtu(hexBytes("1103")));
    std::uint16_t tid = 0;
    auto tcp = bus::modbus::parseTcp(hexBytes("00090000000601030000000a"), &tid);
    ASSERT_TRUE(tcp);
    EXPECT_EQ(tid, 9);
    EXPECT_EQ(tcp->data, hexBytes("0000000a"));
    EXPECT_FALSE(bus::modbus::parseTcp(hexBytes("00090001000601030000000a")));
    EXPECT_FALSE(bus::modbus::parseTcp(hexBytes("000900000006010300")));
    EXPECT_STREQ(bus::modbus::exceptionText(2), "illegal data address");
}

class ModbusModes : public ::testing::TestWithParam<bus::modbus::Mode> {};

TEST_P(ModbusModes, ReadsAndWritesRegistersAndCoils) {
    ModbusRig rig(GetParam());
    auto client = rig.client();
    std::vector<std::uint16_t> regs;
    ASSERT_EQ(client.readHoldingRegisters(17, 0, 3, regs), 0);
    EXPECT_EQ(regs, (std::vector<std::uint16_t>{100, 200, 300}));
    ASSERT_EQ(client.readInputRegisters(17, 1, 2, regs), 0);
    EXPECT_EQ(regs, (std::vector<std::uint16_t>{8, 9}));
    std::vector<bool> bits;
    ASSERT_EQ(client.readCoils(17, 0, 10, bits), 0);
    EXPECT_EQ(bits, (std::vector<bool>{true, false, true, false, false, false, false, false, true, true}));
    ASSERT_EQ(client.readDiscreteInputs(17, 0, 3, bits), 0);
    EXPECT_EQ(bits, (std::vector<bool>{true, true, false}));

    ASSERT_EQ(client.writeRegister(17, 1, 555), 0);
    EXPECT_EQ(rig.slave().holding[1], 555);
    ASSERT_EQ(client.writeRegisters(17, 0, {1, 2, 3}), 0);
    EXPECT_EQ(rig.slave().holding[2], 3);
    ASSERT_EQ(client.writeCoil(17, 1, true), 0);
    EXPECT_TRUE(rig.slave().coils[1]);
    ASSERT_EQ(client.writeCoils(17, 3, {true, true, false, true}), 0);
    EXPECT_TRUE(rig.slave().coils[3]);
    EXPECT_TRUE(rig.slave().coils[4]);
    EXPECT_FALSE(rig.slave().coils[5]);
    EXPECT_TRUE(rig.slave().coils[6]);
    ASSERT_EQ(client.maskWriteRegister(17, 10, 0xFF00, 0x0012), 0);
    EXPECT_EQ(rig.slave().holding[10], 0xFF12);
}

TEST_P(ModbusModes, ExceptionsTimeoutsAndValidation) {
    ModbusRig rig(GetParam());
    auto client = rig.client(150ms);
    std::vector<std::uint16_t> regs;
    EXPECT_EQ(client.readHoldingRegisters(17, 500, 2, regs), -EREMOTEIO);
    EXPECT_EQ(client.lastException(), 2);
    EXPECT_EQ(client.writeRegister(17, 999, 1), -EREMOTEIO);
    EXPECT_EQ(client.readHoldingRegisters(18, 0, 1, regs), -ETIMEDOUT);
    EXPECT_EQ(client.readHoldingRegisters(17, 0, 0, regs), -EINVAL);
    EXPECT_EQ(client.readHoldingRegisters(17, 0, 126, regs), -EINVAL);
    std::vector<std::uint8_t> raw;
    EXPECT_EQ(client.call(17, 0x2B, {}, raw), -EREMOTEIO);
    EXPECT_EQ(client.lastException(), 1);
    ASSERT_EQ(client.readHoldingRegisters(17, 0, 1, regs), 0);
}

INSTANTIATE_TEST_SUITE_P(Framing, ModbusModes, ::testing::Values(bus::modbus::Mode::Rtu, bus::modbus::Mode::Tcp));

TEST(Modbus, FloatRegisterHelpersRoundTrip) {
    const auto [high, low] = bus::modbus::Client::floatToRegisters(123.456F);
    EXPECT_FLOAT_EQ(bus::modbus::Client::registersToFloat(high, low), 123.456F);
    EXPECT_EQ(bus::modbus::Client::floatToRegisters(1.0F).first, 0x3F80);
}

TEST(Nmea, ChecksumsMatchPublishedSentences) {
    EXPECT_EQ(robotics::nmea::checksum("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"), 0x47);
    EXPECT_EQ(robotics::nmea::checksum("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W"), 0x6A);
    EXPECT_EQ(robotics::nmea::build("SD", "DBT", {"8.1", "f", "2.4", "M", "1.3", "F"}), "$SDDBT,8.1,f,2.4,M,1.3,F*0B");
    EXPECT_EQ(robotics::nmea::build("WI", "MWV", {"214.8", "R", "0.1", "K", "A"}), "$WIMWV,214.8,R,0.1,K,A*28");
}

TEST(Nmea, ParsesGgaAndRmcIntoAFix) {
    robotics::nmea::State state;
    auto gga = robotics::nmea::parse("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n");
    ASSERT_TRUE(gga);
    EXPECT_TRUE(gga->checksumValid);
    EXPECT_EQ(gga->talker, "GP");
    EXPECT_EQ(gga->type, "GGA");
    ASSERT_TRUE(state.update(*gga));
    EXPECT_TRUE(state.fix().valid);
    EXPECT_NEAR(state.fix().latitude, 48.1173, 1e-6);
    EXPECT_NEAR(state.fix().longitude, 11.516666667, 1e-6);
    EXPECT_DOUBLE_EQ(state.fix().altitude, 545.4);
    EXPECT_EQ(state.fix().satellites, 8);
    EXPECT_DOUBLE_EQ(state.fix().hdop, 0.9);

    ASSERT_TRUE(state.update(*robotics::nmea::parse("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A")));
    EXPECT_DOUBLE_EQ(state.fix().speedKnots, 22.4);
    EXPECT_DOUBLE_EQ(state.fix().courseDegrees, 84.4);
    EXPECT_EQ(state.fix().utcDate, "230394");
    EXPECT_DOUBLE_EQ(state.fix().magneticVariation, -3.1);
    EXPECT_NEAR(state.toJson().getDouble("latitude"), 48.1173, 1e-6);
}

TEST(Nmea, MarineSentencesAndInvalidInput) {
    robotics::nmea::State state;
    ASSERT_TRUE(state.update(*robotics::nmea::parse("$SDDBT,8.1,f,2.4,M,1.3,F*0B")));
    EXPECT_DOUBLE_EQ(*state.depthMeters, 2.4);
    ASSERT_TRUE(state.update(*robotics::nmea::parse("$WIMWV,214.8,R,0.1,K,A*28")));
    EXPECT_DOUBLE_EQ(*state.windAngle, 214.8);
    EXPECT_NEAR(*state.windSpeed, 0.1 / 1.852, 1e-9);
    ASSERT_TRUE(state.update(*robotics::nmea::parse(robotics::nmea::build("HC", "HDT", {"274.07", "T"}))));
    EXPECT_DOUBLE_EQ(*state.headingTrue, 274.07);
    ASSERT_TRUE(state.update(*robotics::nmea::parse(robotics::nmea::build("II", "MTW", {"17.5", "C"}))));
    EXPECT_DOUBLE_EQ(*state.waterTemperature, 17.5);

    auto bad = robotics::nmea::parse("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00");
    ASSERT_TRUE(bad);
    EXPECT_FALSE(bad->checksumValid);
    robotics::nmea::State untouched;
    EXPECT_FALSE(untouched.update(*bad));
    EXPECT_FALSE(untouched.fix().valid);
    EXPECT_FALSE(robotics::nmea::parse("GPGGA,no dollar"));
    EXPECT_FALSE(robotics::nmea::parse("$GP*4"));
    EXPECT_FALSE(robotics::nmea::parse("$GPGGA,1*ZZ"));
    EXPECT_FALSE(robotics::nmea::parse(""));
    auto noFix = robotics::nmea::parse("$GPGGA,123519,,,,,0,00,,,M,,M,,*66");
    ASSERT_TRUE(noFix);
    robotics::nmea::State s2;
    (void)s2.update(*noFix);
    EXPECT_FALSE(s2.fix().valid);
}

TEST(Nmea, CoordinatesLinesAndFormatting) {
    EXPECT_NEAR(*robotics::nmea::parseCoordinate("4807.038", "N"), 48.1173, 1e-9);
    EXPECT_NEAR(*robotics::nmea::parseCoordinate("01131.000", "W"), -11.5166666667, 1e-9);
    EXPECT_FALSE(robotics::nmea::parseCoordinate("", "N"));
    EXPECT_FALSE(robotics::nmea::parseCoordinate("4807.038", "X"));
    EXPECT_FALSE(robotics::nmea::parseCoordinate("48", "N"));
    EXPECT_EQ(robotics::nmea::formatCoordinate(48.1173, true), "4807.0380,N");
    EXPECT_EQ(robotics::nmea::formatCoordinate(-11.516666667, false), "01131.0000,W");

    robotics::nmea::LineAssembler assembler;
    EXPECT_TRUE(assembler.feed("$GPGGA,1").empty());
    const auto lines = assembler.feed("23*00\r\n$GPRMC,4*11\r\n$GP");
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[0], "$GPGGA,123*00");
    EXPECT_EQ(lines[1], "$GPRMC,4*11");
}

TEST(Adsb, DecodesFramesFromTheReferenceImplementation) {
    using namespace robotics::adsb;
    auto ident = parseHex("8D4840D6202CC371C32CE0576098");
    ASSERT_TRUE(ident);
    EXPECT_TRUE(crcValid(*ident));
    EXPECT_EQ(downlinkFormat(*ident), 17);
    EXPECT_EQ(icao(*ident), 0x4840D6U);
    EXPECT_EQ(typeCode(*ident), 4);
    EXPECT_EQ(decodeIdentification(*ident)->callsign, "KLM1023");

    auto even = *parseHex("8D40621D58C382D690C8AC2863A7");
    auto odd = *parseHex("8D40621D58C386435CC412692AD6");
    EXPECT_TRUE(crcValid(even));
    const auto pe = *decodePosition(even);
    const auto po = *decodePosition(odd);
    EXPECT_EQ(*pe.altitudeFeet, 38000);
    EXPECT_FALSE(pe.odd);
    EXPECT_TRUE(po.odd);
    EXPECT_EQ(pe.cprLatitude, 93000U);
    EXPECT_EQ(pe.cprLongitude, 51372U);
    EXPECT_EQ(po.cprLatitude, 74158U);
    EXPECT_EQ(po.cprLongitude, 50194U);
    const auto pos = cprGlobal(pe.cprLatitude, pe.cprLongitude, po.cprLatitude, po.cprLongitude, false);
    ASSERT_TRUE(pos);
    EXPECT_NEAR(pos->first, 52.25720214843750, 1e-6);
    EXPECT_NEAR(pos->second, 3.91937255859375, 1e-6);
    const auto local = cprLocal(52.258, 3.918, pe.cprLatitude, pe.cprLongitude, false);
    EXPECT_NEAR(local.first, 52.2572, 1e-3);
    EXPECT_NEAR(local.second, 3.9194, 1e-3);

    const auto vel = *decodeVelocity(*parseHex("8D485020994409940838175B284F"));
    EXPECT_EQ(vel.subtype, 1);
    EXPECT_NEAR(*vel.groundSpeedKnots, 159.0, 0.5);
    EXPECT_NEAR(*vel.trackDegrees, 182.88, 0.01);
    EXPECT_EQ(*vel.verticalRateFpm, -832);
}

TEST(Adsb, RejectsCorruptedAndMalformedFrames) {
    using namespace robotics::adsb;
    auto frame = *parseHex("8D4840D6202CC371C32CE0576098");
    frame[6] ^= 0x04;
    EXPECT_FALSE(crcValid(frame));
    Tracker tracker;
    EXPECT_FALSE(tracker.ingest(frame));
    EXPECT_FALSE(tracker.ingestHex("8D4840D6"));
    EXPECT_FALSE(tracker.ingestHex("ZZ4840D6202CC371C32CE0576098"));
    EXPECT_EQ(tracker.rejectedFrames(), 3U);
    EXPECT_FALSE(parseHex(""));
    EXPECT_FALSE(decodePosition(*parseHex("8D4840D6202CC371C32CE0576098")));
    EXPECT_FALSE(decodeVelocity(*parseHex("8D4840D6202CC371C32CE0576098")));
    EXPECT_EQ(cprNl(0.0), 59);
    EXPECT_EQ(cprNl(87.0), 2);
    EXPECT_EQ(cprNl(89.0), 1);
    EXPECT_EQ(cprNl(52.2572), 36);
}

TEST(Adsb, TrackerFusesIdentityPositionAndVelocity) {
    using namespace robotics::adsb;
    Tracker tracker;
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(tracker.ingestHex("8D4840D6202CC371C32CE0576098", t0));
    ASSERT_TRUE(tracker.ingestHex("8D40621D58C382D690C8AC2863A7", t0));
    ASSERT_TRUE(tracker.ingestHex("8D40621D58C386435CC412692AD6", t0 + 1s));
    ASSERT_TRUE(tracker.ingestHex("*8D485020994409940838175B284F;", t0 + 2s));
    const auto klm = tracker.find(0x4840D6);
    ASSERT_TRUE(klm);
    EXPECT_EQ(klm->callsign, "KLM1023");
    const auto plane = tracker.find(0x40621D);
    ASSERT_TRUE(plane);
    EXPECT_EQ(*plane->altitudeFeet, 38000);
    EXPECT_NEAR(*plane->latitude, 52.26578017412606, 1e-9);
    EXPECT_NEAR(*plane->longitude, 3.938912527901786, 1e-9);
    EXPECT_EQ(tracker.aircraft().size(), 3U);
    EXPECT_EQ(tracker.toJson().find("aircraft")->asArray().size(), 3U);
    tracker.expire(1s, t0 + 3s);
    EXPECT_EQ(tracker.aircraft().size(), 1U);
}

TEST(Adsb, ReadsAvrStreamsAndSbsLines) {
    using namespace robotics::adsb;
    Tracker tracker;
    EXPECT_EQ(tracker.ingestAvrStream("*8D4840D6202CC371C32CE0576098;\n*8D40621D58C3"), 1U);
    EXPECT_EQ(tracker.ingestAvrStream("82D690C8AC2863A7;\n*junk;\n"), 1U);
    EXPECT_EQ(tracker.aircraft().size(), 2U);
    EXPECT_TRUE(
        tracker.ingestSbs("MSG,3,1,1,ABCDEF,1,2026/01/01,00:00:00.000,2026/01/01,00:00:00.000,,35000,,,47.1234,8.5678,,,0,0,0,0\r\n"));
    const auto sbs = tracker.find(0xABCDEF);
    ASSERT_TRUE(sbs);
    EXPECT_EQ(*sbs->altitudeFeet, 35000);
    EXPECT_NEAR(*sbs->latitude, 47.1234, 1e-9);
    EXPECT_FALSE(tracker.ingestSbs("STA,1,2"));
    EXPECT_FALSE(tracker.ingestSbs("MSG,3,1,1,XYZ"));
    EXPECT_TRUE(tracker.ingestSbs("MSG,1,1,1,ABCDEF,1,d,t,d,t,UAL123  ,,,,,,,,0,0,0,0"));
    EXPECT_EQ(tracker.find(0xABCDEF)->callsign, "UAL123");
}

TEST(Arinc429, LabelsParityAndDataFormats) {
    using namespace bus::arinc429;
    EXPECT_EQ(labelToOctal(labelFromOctal(203)), 203U);
    EXPECT_EQ(labelToOctal(labelFromOctal(320)), 320U);
    EXPECT_EQ(labelFromOctal(1), 0x80);
    EXPECT_EQ(labelFromOctal(377), 0xFF);
    for (const unsigned octal : {203U, 205U, 310U, 320U, 361U}) {
        Word w;
        w.label = labelFromOctal(octal);
        w.sdi = 2;
        w.data = 0x2A5A5;
        w.ssm = 3;
        const auto raw = encode(w);
        EXPECT_TRUE(parityOk(raw));
        const auto back = decode(raw);
        ASSERT_TRUE(back);
        EXPECT_EQ(labelToOctal(back->label), octal);
        EXPECT_EQ(back->sdi, 2);
        EXPECT_EQ(back->data, 0x2A5A5U);
        EXPECT_EQ(back->ssm, 3);
        EXPECT_FALSE(decode(raw ^ 0x100));
        EXPECT_TRUE(decode(raw ^ 0x100, false));
    }
}

TEST(Arinc429, BnrBcdAndWordStreams) {
    using namespace bus::arinc429;
    for (const double v : {0.0, 45.0, -45.0, 179.99, -180.0}) {
        const auto data = encodeBnr(v, 15, 180.0);
        EXPECT_NEAR(decodeBnr(data, 15, 180.0), v, 180.0 / 16384.0);
    }
    EXPECT_EQ(encodeBnr(1e9, 15, 180.0), 0x3FFFU);
    EXPECT_EQ(encodeBcd(4, 1234), 0x1234U);
    EXPECT_EQ(*decodeBcd(0x1234, 4), 1234U);
    EXPECT_FALSE(decodeBcd(0x12F4, 4));
    const std::vector<std::uint8_t> bytes = {0x80, 0x00, 0x00, 0x01, 0x01, 0x02, 0x03, 0x04, 0xFF};
    EXPECT_EQ(readWords(bytes, true), (std::vector<std::uint32_t>{0x80000001U, 0x01020304U}));
    EXPECT_EQ(readWords(bytes, false), (std::vector<std::uint32_t>{0x01000080U, 0x04030201U}));
}
