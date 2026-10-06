#include <gygax/bus/j1939.hpp>

#include <algorithm>
#include <cmath>

namespace gygax::bus::j1939 {

namespace {

constexpr std::size_t kJ1939DataBytes = 8;
constexpr std::uint8_t kFirstUnavailableByteValue = 0xFE;
constexpr std::uint16_t kFirstUnavailableWordValue = 0xFB00;
constexpr std::uint8_t kPriorityBitMask = 0x7;
constexpr std::uint8_t kMaximumPeerToPeerFormatValue = 240;
constexpr std::uint32_t kPgnDataPageMask = 0x3;
constexpr std::uint32_t kPgnMask = 0x3FF00U;
constexpr std::uint32_t kByteMask = 0xFFU;
constexpr int kPriorityFieldShift = 26;
constexpr int kPgnFormatFieldShift = 8;
constexpr int kPgnPageFieldShift = 24;
constexpr int kPgnSourceFieldShift = 16;
constexpr int kPgnDestinationFieldShift = 8;
constexpr std::size_t kEngineSpeedByteOffset = 3;
constexpr double kTorqueRawOffsetPercent = 125.0;
constexpr long kTorqueRawMaximum = 250L;
constexpr double kMaximumEncodableEngineSpeedRpm = 8031.875;
constexpr double kEngineSpeedResolutionRpm = 0.125;
constexpr double kWheelSpeedCountsPerKph = 256.0;
constexpr double kTemperatureRawOffsetCelsius = 40.0;
constexpr double kTemperatureRawOffsetKelvin = 273.0;
constexpr double kOilTemperatureResolutionCelsius = 0.03125;
constexpr double kFuelRateResolutionLitersPerHour = 0.05;
constexpr double kThrottlePositionResolutionPercent = 0.4;
constexpr double kBatteryVoltageResolutionVolts = 0.05;
constexpr double kBarometricPressureResolutionKpa = 0.5;
constexpr std::uint8_t kUnavailableDataByte = 0xFF;
constexpr long kMaximumWheelSpeedRawValue = 0xFAFFL;

bool available8(std::uint8_t v) {
    return v < kFirstUnavailableByteValue;
}
bool available16(std::uint16_t v) {
    return v < kFirstUnavailableWordValue;
}

std::uint16_t word(std::span<const std::uint8_t> d, std::size_t i) {
    return static_cast<std::uint16_t>(d[i] | (d[i + 1] << 8));
}

} // namespace

std::uint32_t Id::encode() const {
    std::uint32_t id = static_cast<std::uint32_t>(priority & kPriorityBitMask) << kPriorityFieldShift;
    const std::uint8_t pf = static_cast<std::uint8_t>((pgn >> kPgnFormatFieldShift) & kByteMask);
    id |= (pgn & kPgnMask) << 8;
    if (pf < kMaximumPeerToPeerFormatValue)
        id |= static_cast<std::uint32_t>(destination) << kPgnDestinationFieldShift;
    else
        id |= (pgn & kByteMask) << 8;
    id |= source;
    return id;
}

Id Id::decode(std::uint32_t identifier) {
    Id out;
    out.priority = static_cast<std::uint8_t>((identifier >> kPriorityFieldShift) & kPriorityBitMask);
    const std::uint8_t pf = static_cast<std::uint8_t>((identifier >> kPgnSourceFieldShift) & kByteMask);
    const std::uint8_t ps = static_cast<std::uint8_t>((identifier >> kPgnDestinationFieldShift) & kByteMask);
    out.source = static_cast<std::uint8_t>(identifier & kByteMask);
    const std::uint32_t dataPage = (identifier >> kPgnPageFieldShift) & kPgnDataPageMask;
    if (pf < kMaximumPeerToPeerFormatValue) {
        out.pgn = (dataPage << 16) | (static_cast<std::uint32_t>(pf) << 8);
        out.destination = ps;
    } else {
        out.pgn = (dataPage << 16) | (static_cast<std::uint32_t>(pf) << 8) | ps;
        out.destination = kGlobalAddress;
    }
    return out;
}

CanFrame makeFrame(const Id& id, std::span<const std::uint8_t> data) {
    return CanFrame::make(id.encode(), data, true);
}

CanFrame makeRequest(std::uint32_t requestedPgn, std::uint8_t destination, std::uint8_t source) {
    const std::uint8_t bytes[3] = {static_cast<std::uint8_t>(requestedPgn & 0xFF), static_cast<std::uint8_t>((requestedPgn >> 8) & 0xFF),
                                   static_cast<std::uint8_t>((requestedPgn >> 16) & 0xFF)};
    return makeFrame(Id{kDefaultPriority, kPgnRequest, destination, source}, bytes);
}

std::vector<Signal> decode(std::uint32_t pgn, std::span<const std::uint8_t> d) {
    std::vector<Signal> out;
    if (d.size() < kJ1939DataBytes) return out;
    switch (pgn) {
    case kPgnEec1:
        if (available8(d[1])) out.push_back({"driver_demand_torque", d[1] - kTorqueRawOffsetPercent, "%"});
        if (available8(d[2])) out.push_back({"actual_engine_torque", d[2] - kTorqueRawOffsetPercent, "%"});
        if (available16(word(d, kEngineSpeedByteOffset)))
            out.push_back({"engine_speed", word(d, kEngineSpeedByteOffset) * kEngineSpeedResolutionRpm, "rpm"});
        break;
    case kPgnCcvs:
        if (available16(word(d, 1)))
            out.push_back({"wheel_based_speed", word(d, 1) / kWheelSpeedCountsPerKph, "km/h"});
        break;
    case kPgnEt1:
        if (available8(d[0])) out.push_back({"coolant_temperature", d[0] - kTemperatureRawOffsetCelsius, "degC"});
        if (available8(d[1])) out.push_back({"fuel_temperature", d[1] - kTemperatureRawOffsetCelsius, "degC"});
        if (available16(word(d, 2)))
            out.push_back({"oil_temperature", word(d, 2) * kOilTemperatureResolutionCelsius - kTemperatureRawOffsetKelvin, "degC"});
        break;
    case kPgnLfe:
        if (available16(word(d, 0)))
            out.push_back({"fuel_rate", word(d, 0) * kFuelRateResolutionLitersPerHour, "L/h"});
        if (available8(d[6])) out.push_back({"throttle_position", d[6] * kThrottlePositionResolutionPercent, "%"});
        break;
    case kPgnVep1:
        if (available16(word(d, 4)))
            out.push_back({"battery_potential", word(d, 4) * kBatteryVoltageResolutionVolts, "V"});
        break;
    case kPgnAmbient:
        if (available8(d[0])) out.push_back({"barometric_pressure", d[0] * kBarometricPressureResolutionKpa, "kPa"});
        if (available16(word(d, 3)))
            out.push_back({"ambient_temperature", word(d, 3) * kOilTemperatureResolutionCelsius - kTemperatureRawOffsetKelvin,
                           "degC"});
        break;
    default: break;
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> encodeEec1(double rpm, double torquePercent) {
    if (rpm < 0.0 || rpm > kMaximumEncodableEngineSpeedRpm) return std::nullopt;
    std::vector<std::uint8_t> d(kJ1939DataBytes, kUnavailableDataByte);
    const auto torque = static_cast<std::uint8_t>(
        std::clamp(std::lround(torquePercent + kTorqueRawOffsetPercent), 0L, kTorqueRawMaximum));
    d[1] = torque;
    d[2] = torque;
    const auto raw = static_cast<std::uint16_t>(std::lround(rpm / kEngineSpeedResolutionRpm));
    d[3] = static_cast<std::uint8_t>(raw & 0xFF);
    d[4] = static_cast<std::uint8_t>(raw >> 8);
    return d;
}

std::vector<std::uint8_t> encodeCcvs(double speedKmh) {
    std::vector<std::uint8_t> d(kJ1939DataBytes, kUnavailableDataByte);
    const auto raw = static_cast<std::uint16_t>(
        std::clamp(std::lround(speedKmh * kWheelSpeedCountsPerKph), 0L, kMaximumWheelSpeedRawValue));
    d[1] = static_cast<std::uint8_t>(raw & 0xFF);
    d[2] = static_cast<std::uint8_t>(raw >> 8);
    return d;
}

} // namespace gygax::bus::j1939
