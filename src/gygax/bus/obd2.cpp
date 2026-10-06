#include <gygax/bus/obd2.hpp>
#include <gygax/core/posix.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace gygax::bus {

namespace {

struct PidSpec {
    std::uint8_t pid;
    const char* name;
    const char* unit;
    std::uint8_t bytes;
};

constexpr PidSpec kPids[] = {
    {0x04, "engine_load", "%", 1},
    {0x05, "coolant_temperature", "degC", 1},
    {0x0A, "fuel_pressure", "kPa", 1},
    {0x0B, "intake_manifold_pressure", "kPa", 1},
    {0x0C, "engine_speed", "rpm", 2},
    {0x0D, "vehicle_speed", "km/h", 1},
    {0x0F, "intake_air_temperature", "degC", 1},
    {0x10, "mass_air_flow", "g/s", 2},
    {0x11, "throttle_position", "%", 1},
    {0x2F, "fuel_level", "%", 1},
    {0x33, "barometric_pressure", "kPa", 1},
    {0x42, "control_module_voltage", "V", 2},
    {0x46, "ambient_air_temperature", "degC", 1},
    {0x5C, "engine_oil_temperature", "degC", 1},
};

const PidSpec* findSpec(std::uint8_t pid) {
    for (const auto& s : kPids) {
        if (s.pid == pid) return &s;
    }
    return nullptr;
}

} // namespace

const char* Obd2Client::pidName(std::uint8_t pid) {
    const auto* s = findSpec(pid);
    return s == nullptr ? "unknown" : s->name;
}

std::optional<Obd2Value> Obd2Client::decode(std::uint8_t pid, std::span<const std::uint8_t> data) {
    const auto* spec = findSpec(pid);
    if (spec == nullptr || data.size() < spec->bytes) return std::nullopt;
    const double a = data[0];
    const double b = data.size() > 1 ? data[1] : 0.0;
    double v = 0.0;
    switch (pid) {
    case 0x04:
    case 0x11:
    case 0x2F: v = a * 100.0 / 255.0; break;
    case 0x05:
    case 0x0F:
    case 0x46:
    case 0x5C: v = a - 40.0; break;
    case 0x0A: v = a * 3.0; break;
    case 0x0B:
    case 0x0D:
    case 0x33: v = a; break;
    case 0x0C: v = (256.0 * a + b) / 4.0; break;
    case 0x10: v = (256.0 * a + b) / 100.0; break;
    case 0x42: v = (256.0 * a + b) / 1000.0; break;
    default: return std::nullopt;
    }
    return Obd2Value{pid, spec->name, v, spec->unit};
}

std::optional<std::vector<std::uint8_t>> VirtualObdEcu::encode(std::uint8_t pid, double value) {
    auto clampByte = [](double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); };
    auto word = [](double v) {
        const auto raw = static_cast<std::uint16_t>(std::clamp(std::lround(v), 0L, 65535L));
        return std::vector<std::uint8_t>{static_cast<std::uint8_t>(raw >> 8), static_cast<std::uint8_t>(raw & 0xFF)};
    };
    switch (pid) {
    case 0x04:
    case 0x11:
    case 0x2F: return std::vector<std::uint8_t>{clampByte(value * 255.0 / 100.0)};
    case 0x05:
    case 0x0F:
    case 0x46:
    case 0x5C: return std::vector<std::uint8_t>{clampByte(value + 40.0)};
    case 0x0A: return std::vector<std::uint8_t>{clampByte(value / 3.0)};
    case 0x0B:
    case 0x0D:
    case 0x33: return std::vector<std::uint8_t>{clampByte(value)};
    case 0x0C: return word(value * 4.0);
    case 0x10: return word(value * 100.0);
    case 0x42: return word(value * 1000.0);
    default: return std::nullopt;
    }
}

std::string Obd2Client::formatDtc(std::uint8_t high, std::uint8_t low) {
    static constexpr char systems[] = {'P', 'C', 'B', 'U'};
    return std::format("{}{}{:X}{:02X}", systems[high >> 6], (high >> 4) & 0x3, high & 0x0F, low);
}

Obd2Client::Obd2Client(std::shared_ptr<CanBus> bus, std::uint8_t ecuIndex, std::chrono::milliseconds timeout)
    : channel_(std::move(bus), IsoTpOptions{static_cast<std::uint32_t>(0x7E0 + ecuIndex), static_cast<std::uint32_t>(0x7E8 + ecuIndex),
                                            false, false, true, 0xCC, 0, std::chrono::microseconds(0), timeout, 8}),
      timeout_(timeout) {}

IOResult Obd2Client::request(std::uint8_t mode, std::span<const std::uint8_t> arguments, std::vector<std::uint8_t>& response) {
    std::vector<std::uint8_t> req{mode};
    req.insert(req.end(), arguments.begin(), arguments.end());
    if (const auto rc = channel_.send(req); rc != 0) return rc;
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<std::uint8_t> reply;
        const auto rc =
            channel_.receive(reply, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
        if (rc != 0) return rc;
        if (reply.size() >= 3 && reply[0] == 0x7F && reply[1] == mode) {
            if (reply[2] == 0x78) continue;
            return -EREMOTEIO;
        }
        if (!reply.empty() && reply[0] == static_cast<std::uint8_t>(mode + 0x40)) {
            response.assign(reply.begin() + 1, reply.end());
            return 0;
        }
    }
    return -ETIMEDOUT;
}

std::optional<Obd2Value> Obd2Client::readPid(std::uint8_t pid) {
    std::vector<std::uint8_t> reply;
    const std::uint8_t args[1] = {pid};
    if (request(0x01, args, reply) != 0 || reply.size() < 2 || reply[0] != pid) return std::nullopt;
    return decode(pid, std::span<const std::uint8_t>(reply).subspan(1));
}

std::vector<std::uint8_t> Obd2Client::supportedPids() {
    std::vector<std::uint8_t> supported;
    for (unsigned base = 0x00; base <= 0xE0; base += 0x20) {
        std::vector<std::uint8_t> reply;
        const std::uint8_t args[1] = {static_cast<std::uint8_t>(base)};
        if (request(0x01, args, reply) != 0 || reply.size() < 5 || reply[0] != base) break;
        std::uint32_t mask = (static_cast<std::uint32_t>(reply[1]) << 24) | (static_cast<std::uint32_t>(reply[2]) << 16) |
                             (static_cast<std::uint32_t>(reply[3]) << 8) | reply[4];
        for (unsigned bit = 0; bit < 32; ++bit) {
            if ((mask & (0x80000000U >> bit)) != 0) supported.push_back(static_cast<std::uint8_t>(base + bit + 1));
        }
        if ((mask & 1U) == 0) break;
    }
    return supported;
}

std::vector<std::string> Obd2Client::readStoredDtcs() {
    std::vector<std::string> codes;
    std::vector<std::uint8_t> reply;
    if (request(0x03, {}, reply) != 0 || reply.empty()) return codes;
    const std::size_t count = reply[0];
    for (std::size_t i = 0; i < count && 1 + 2 * i + 1 < reply.size(); ++i) {
        const auto high = reply[1 + 2 * i];
        const auto low = reply[2 + 2 * i];
        if (high == 0 && low == 0) continue;
        codes.push_back(formatDtc(high, low));
    }
    return codes;
}

std::string Obd2Client::readVin() {
    std::vector<std::uint8_t> reply;
    const std::uint8_t args[1] = {0x02};
    if (request(0x09, args, reply) != 0 || reply.size() < 3 || reply[0] != 0x02) return {};
    std::string vin(reply.begin() + 2, reply.end());
    while (!vin.empty() && (vin.back() == '\0' || vin.back() == ' ')) vin.pop_back();
    return vin;
}

VirtualObdEcu::VirtualObdEcu(std::shared_ptr<CanBus> bus, std::uint8_t ecuIndex, PidSource source)
    : channel_(std::move(bus), IsoTpOptions{static_cast<std::uint32_t>(0x7E8 + ecuIndex), static_cast<std::uint32_t>(0x7E0 + ecuIndex),
                                            false, false, true, 0x55, 0, std::chrono::microseconds(0), std::chrono::milliseconds(500), 8}),
      source_(std::move(source)) {}

VirtualObdEcu::~VirtualObdEcu() {
    stop();
}

void VirtualObdEcu::start() {
    if (worker_.joinable()) return;
    worker_ = std::jthread([this](const std::stop_token& st) {
        while (!st.stop_requested()) serveOne(std::chrono::milliseconds(50));
    });
}

void VirtualObdEcu::stop() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

bool VirtualObdEcu::serveOne(std::chrono::milliseconds timeout) {
    std::vector<std::uint8_t> req;
    if (channel_.receive(req, timeout) != 0 || req.empty()) return false;
    const std::uint8_t mode = req[0];
    std::vector<std::uint8_t> reply;
    auto negative = [&](std::uint8_t code) { reply = {0x7F, mode, code}; };

    if (mode == 0x01 && req.size() >= 2) {
        const std::uint8_t pid = req[1];
        if (pid % 0x20 == 0) {
            auto supports = [&](unsigned candidate) {
                return source_ && candidate < 0x100 && encode(static_cast<std::uint8_t>(candidate), 0).has_value() &&
                       source_(static_cast<std::uint8_t>(candidate)).has_value();
            };
            std::uint32_t mask = 0;
            for (unsigned bit = 0; bit < 32; ++bit) {
                const unsigned candidate = pid + bit + 1;
                bool has = false;
                if (candidate % 0x20 == 0) {
                    for (unsigned next = candidate + 1; next <= candidate + 0x20 && !has; ++next) has = supports(next);
                } else {
                    has = supports(candidate);
                }
                if (has) mask |= 0x80000000U >> bit;
            }
            reply = {static_cast<std::uint8_t>(mode + 0x40), pid,
                     static_cast<std::uint8_t>(mask >> 24),  static_cast<std::uint8_t>(mask >> 16),
                     static_cast<std::uint8_t>(mask >> 8),   static_cast<std::uint8_t>(mask)};
        } else if (auto value = source_ ? source_(pid) : std::nullopt) {
            if (auto bytes = encode(pid, *value)) {
                reply = {static_cast<std::uint8_t>(mode + 0x40), pid};
                reply.insert(reply.end(), bytes->begin(), bytes->end());
            } else {
                negative(0x31);
            }
        } else {
            negative(0x31);
        }
    } else if (mode == 0x03) {
        reply = {static_cast<std::uint8_t>(mode + 0x40), static_cast<std::uint8_t>(dtcs_.size())};
        for (const auto code : dtcs_) {
            reply.push_back(static_cast<std::uint8_t>(code >> 8));
            reply.push_back(static_cast<std::uint8_t>(code & 0xFF));
        }
    } else if (mode == 0x09 && req.size() >= 2 && req[1] == 0x02) {
        reply = {static_cast<std::uint8_t>(mode + 0x40), 0x02, 0x01};
        reply.insert(reply.end(), vin_.begin(), vin_.end());
    } else {
        negative(0x11);
    }
    ++served_;
    return channel_.send(reply) == 0;
}

} // namespace gygax::bus
