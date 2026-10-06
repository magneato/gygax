#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gygax/bus/can.hpp>

namespace gygax::bus {

struct DbcSignal {
    std::string name;
    std::uint32_t startBit = 0;
    std::uint32_t length = 0;
    bool littleEndian = true;
    bool isSigned = false;
    double factor = 1.0;
    double offset = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    std::string unit;
    bool isMultiplexer = false;
    std::optional<std::uint32_t> multiplexValue;
    std::map<std::int64_t, std::string> choices;
};

struct DbcMessage {
    std::uint32_t id = 0;
    bool extended = false;
    std::string name;
    std::uint32_t length = 0;
    std::string sender;
    std::vector<DbcSignal> signals;

    [[nodiscard]] const DbcSignal* find(std::string_view signal) const;
};

struct DecodedSignal {
    std::string name;
    double value = 0.0;
    std::int64_t raw = 0;
    std::string unit;
    std::string choice;
};

class DbcDatabase {
public:
    static std::optional<DbcDatabase> parse(std::string_view text, std::string* error = nullptr);

    [[nodiscard]] const DbcMessage* byId(std::uint32_t id, bool extended) const;
    [[nodiscard]] const DbcMessage* byName(std::string_view name) const;
    [[nodiscard]] const std::vector<DbcMessage>& messages() const { return messages_; }

    [[nodiscard]] std::vector<DecodedSignal> decode(const CanFrame& frame) const;
    [[nodiscard]] std::optional<CanFrame> encode(std::string_view messageName, const std::map<std::string, double>& values,
                                                 std::string* error = nullptr) const;

    static std::int64_t extractRaw(std::span<const std::uint8_t> data, const DbcSignal& signal);
    static void insertRaw(std::span<std::uint8_t> data, const DbcSignal& signal, std::int64_t raw);

private:
    std::vector<DbcMessage> messages_;
};

} // namespace gygax::bus
