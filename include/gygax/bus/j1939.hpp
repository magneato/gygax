#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gygax/bus/can.hpp>

namespace gygax::bus::j1939 {

inline constexpr std::uint8_t kDefaultPriority = 6;
inline constexpr std::uint8_t kGlobalAddress = 0xFF;
inline constexpr std::uint8_t kNullAddress = 0xFE;

struct Id {
    std::uint8_t priority = kDefaultPriority;
    std::uint32_t pgn = 0;
    std::uint8_t destination = kGlobalAddress;
    std::uint8_t source = kNullAddress;

    [[nodiscard]] std::uint32_t encode() const;
    [[nodiscard]] bool isBroadcast() const { return destination == kGlobalAddress; }
    static Id decode(std::uint32_t identifier);
};

struct Signal {
    std::string name;
    double value = 0.0;
    std::string unit;
};

constexpr std::uint32_t kPgnEec1 = 61444;
constexpr std::uint32_t kPgnEt1 = 65262;
constexpr std::uint32_t kPgnLfe = 65266;
constexpr std::uint32_t kPgnCcvs = 65265;
constexpr std::uint32_t kPgnVep1 = 65271;
constexpr std::uint32_t kPgnAmbient = 65269;
constexpr std::uint32_t kPgnRequest = 59904;

std::vector<Signal> decode(std::uint32_t pgn, std::span<const std::uint8_t> data);
CanFrame makeFrame(const Id& id, std::span<const std::uint8_t> data);
CanFrame makeRequest(std::uint32_t requestedPgn, std::uint8_t destination, std::uint8_t source);
std::optional<std::vector<std::uint8_t>> encodeEec1(double rpm, double torquePercent);
std::vector<std::uint8_t> encodeCcvs(double speedKmh);

} // namespace gygax::bus::j1939
