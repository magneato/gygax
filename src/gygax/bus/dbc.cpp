#include <gygax/bus/dbc.hpp>

#include <algorithm>
#include <charconv>

#include <gygax/core/charconv.hpp>
#include <cmath>
#include <format>
#include <regex>
#include <sstream>

namespace gygax::bus {

namespace {

constexpr std::uint32_t kExtendedFlag = 0x80000000U;
constexpr std::uint32_t kMaxSignalBitLength = 64;
constexpr std::uint32_t kMaxCanPayloadBits = kMaxCanFdPayloadBytes * 8;

double toDouble(const std::string& s) {
    double v = 0.0;
    gygax::fromChars(s.data(), s.data() + s.size(), v);
    return v;
}

} // namespace

const DbcSignal* DbcMessage::find(std::string_view signal) const {
    for (const auto& s : signals) {
        if (s.name == signal) return &s;
    }
    return nullptr;
}

std::optional<DbcDatabase> DbcDatabase::parse(std::string_view text, std::string* error) {
    static const std::regex messageRe(R"(^BO_\s+(\d+)\s+(\w+)\s*:\s*(\d+)\s+(\w+))");
    static const std::regex signalRe(
        R"re(^\s*SG_\s+(\w+)\s*(M|m\d+M?|m\d+)?\s*:\s*(\d+)\|(\d+)@([01])([+-])\s*\(\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\)\s*\[\s*([-+0-9.eE]+)\s*\|\s*([-+0-9.eE]+)\s*\]\s*"([^"]*)")re");
    static const std::regex valRe(R"(^VAL_\s+(\d+)\s+(\w+)\s+(.*);\s*$)");
    static const std::regex choiceRe(R"re((-?\d+)\s+"([^"]*)")re");

    DbcDatabase db;
    std::istringstream in{std::string(text)};
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::smatch m;
        if (std::regex_search(line, m, messageRe)) {
            DbcMessage msg;
            const auto rawId = static_cast<std::uint64_t>(std::stoull(m[1].str()));
            msg.extended = (rawId & kExtendedFlag) != 0;
            msg.id = static_cast<std::uint32_t>(rawId & (msg.extended ? kExtendedCanMaxIdentifier : kStandardCanMaxIdentifier));
            msg.name = m[2].str();
            msg.length = static_cast<std::uint32_t>(std::stoul(m[3].str()));
            msg.sender = m[4].str();
            if (msg.length > kMaxCanFdPayloadBytes) {
                if (error != nullptr)
                    *error = std::format("line {}: message '{}' longer than {} bytes", lineNo, msg.name, kMaxCanFdPayloadBytes);
                return std::nullopt;
            }
            db.messages_.push_back(std::move(msg));
            continue;
        }
        if (std::regex_search(line, m, signalRe)) {
            if (db.messages_.empty()) {
                if (error != nullptr) *error = std::format("line {}: signal before any message", lineNo);
                return std::nullopt;
            }
            DbcSignal s;
            s.name = m[1].str();
            const auto mux = m[2].str();
            if (mux == "M") {
                s.isMultiplexer = true;
            } else if (!mux.empty() && mux[0] == 'm') {
                s.multiplexValue = static_cast<std::uint32_t>(std::stoul(mux.substr(1)));
            }
            s.startBit = static_cast<std::uint32_t>(std::stoul(m[3].str()));
            s.length = static_cast<std::uint32_t>(std::stoul(m[4].str()));
            s.littleEndian = m[5].str() == "1";
            s.isSigned = m[6].str() == "-";
            s.factor = toDouble(m[7].str());
            s.offset = toDouble(m[8].str());
            s.minimum = toDouble(m[9].str());
            s.maximum = toDouble(m[10].str());
            s.unit = m[11].str();
            if (s.length == 0 || s.length > kMaxSignalBitLength || s.startBit >= kMaxCanPayloadBits) {
                if (error != nullptr) *error = std::format("line {}: invalid signal geometry for '{}'", lineNo, s.name);
                return std::nullopt;
            }
            db.messages_.back().signals.push_back(std::move(s));
            continue;
        }
        if (std::regex_search(line, m, valRe)) {
            const auto rawId = static_cast<std::uint64_t>(std::stoull(m[1].str()));
            const bool ext = (rawId & kExtendedFlag) != 0;
            const auto id = static_cast<std::uint32_t>(rawId & (ext ? kExtendedCanMaxIdentifier : kStandardCanMaxIdentifier));
            for (auto& msg : db.messages_) {
                if (msg.id != id || msg.extended != ext) continue;
                for (auto& sig : msg.signals) {
                    if (sig.name != m[2].str()) continue;
                    const std::string tail = m[3].str();
                    for (std::sregex_iterator it(tail.begin(), tail.end(), choiceRe), end; it != end; ++it)
                        sig.choices[std::stoll((*it)[1].str())] = (*it)[2].str();
                }
            }
        }
    }
    if (db.messages_.empty()) {
        if (error != nullptr) *error = "no messages found";
        return std::nullopt;
    }
    return db;
}

const DbcMessage* DbcDatabase::byId(std::uint32_t id, bool extended) const {
    for (const auto& m : messages_) {
        if (m.id == id && m.extended == extended) return &m;
    }
    return nullptr;
}

const DbcMessage* DbcDatabase::byName(std::string_view name) const {
    for (const auto& m : messages_) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

std::int64_t DbcDatabase::extractRaw(std::span<const std::uint8_t> data, const DbcSignal& s) {
    std::uint64_t value = 0;
    if (s.littleEndian) {
        for (std::uint32_t i = 0; i < s.length; ++i) {
            const std::uint32_t bit = s.startBit + i;
            const std::size_t byte = bit / 8;
            if (byte >= data.size()) break;
            if ((data[byte] >> (bit % 8)) & 1U) value |= 1ULL << i;
        }
    } else {
        std::size_t byte = s.startBit / 8;
        int bit = static_cast<int>(s.startBit % 8);
        for (std::uint32_t i = 0; i < s.length; ++i) {
            const bool set = byte < data.size() && ((data[byte] >> bit) & 1U) != 0;
            value = (value << 1) | (set ? 1U : 0U);
            if (bit == 0) {
                bit = 7;
                ++byte;
            } else {
                --bit;
            }
        }
    }
    if (s.isSigned && s.length > 0 && s.length < 64 && (value & (1ULL << (s.length - 1))) != 0) value |= ~0ULL << s.length;
    return static_cast<std::int64_t>(value);
}

void DbcDatabase::insertRaw(std::span<std::uint8_t> data, const DbcSignal& s, std::int64_t raw) {
    const auto value = static_cast<std::uint64_t>(raw);
    if (s.littleEndian) {
        for (std::uint32_t i = 0; i < s.length; ++i) {
            const std::uint32_t bit = s.startBit + i;
            const std::size_t byte = bit / 8;
            if (byte >= data.size()) break;
            const auto mask = static_cast<std::uint8_t>(1U << (bit % 8));
            if ((value >> i) & 1U)
                data[byte] |= mask;
            else
                data[byte] &= static_cast<std::uint8_t>(~mask);
        }
    } else {
        std::size_t byte = s.startBit / 8;
        int bit = static_cast<int>(s.startBit % 8);
        for (std::uint32_t i = 0; i < s.length; ++i) {
            const bool set = ((value >> (s.length - 1 - i)) & 1U) != 0;
            if (byte < data.size()) {
                const auto mask = static_cast<std::uint8_t>(1U << bit);
                if (set)
                    data[byte] |= mask;
                else
                    data[byte] &= static_cast<std::uint8_t>(~mask);
            }
            if (bit == 0) {
                bit = 7;
                ++byte;
            } else {
                --bit;
            }
        }
    }
}

std::vector<DecodedSignal> DbcDatabase::decode(const CanFrame& frame) const {
    std::vector<DecodedSignal> out;
    const auto* msg = byId(frame.id, frame.extended);
    if (msg == nullptr) return out;
    std::optional<std::int64_t> muxValue;
    for (const auto& s : msg->signals) {
        if (s.isMultiplexer) muxValue = extractRaw(frame.payload(), s);
    }
    for (const auto& s : msg->signals) {
        if (s.multiplexValue && (!muxValue || static_cast<std::int64_t>(*s.multiplexValue) != *muxValue)) continue;
        DecodedSignal d;
        d.name = s.name;
        d.raw = extractRaw(frame.payload(), s);
        d.value = static_cast<double>(d.raw) * s.factor + s.offset;
        d.unit = s.unit;
        if (auto it = s.choices.find(d.raw); it != s.choices.end()) d.choice = it->second;
        out.push_back(std::move(d));
    }
    return out;
}

std::optional<CanFrame> DbcDatabase::encode(std::string_view messageName, const std::map<std::string, double>& values,
                                            std::string* error) const {
    const auto* msg = byName(messageName);
    if (msg == nullptr) {
        if (error != nullptr) *error = "unknown message '" + std::string(messageName) + "'";
        return std::nullopt;
    }
    CanFrame frame;
    frame.id = msg->id;
    frame.extended = msg->extended;
    frame.fd = msg->length > kClassicCanPayloadBytes;
    frame.length = static_cast<std::uint8_t>(msg->length);
    for (const auto& [name, value] : values) {
        const auto* s = msg->find(name);
        if (s == nullptr) {
            if (error != nullptr) *error = "message '" + msg->name + "' has no signal '" + name + "'";
            return std::nullopt;
        }
        if (!std::isfinite(value)) {
            if (error != nullptr) *error = "signal '" + name + "' is not finite";
            return std::nullopt;
        }
        auto raw = static_cast<std::int64_t>(std::llround((value - s->offset) / s->factor));
        const std::int64_t maxRaw = s->isSigned ? (s->length >= kMaxSignalBitLength ? INT64_MAX : (1LL << (s->length - 1)) - 1)
                                                : (s->length >= 63 ? INT64_MAX : (1LL << s->length) - 1);
        const std::int64_t minRaw = s->isSigned ? (s->length >= kMaxSignalBitLength ? INT64_MIN : -(1LL << (s->length - 1))) : 0;
        raw = std::clamp(raw, minRaw, maxRaw);
        insertRaw({frame.data.data(), frame.length}, *s, raw);
    }
    return frame;
}

} // namespace gygax::bus
