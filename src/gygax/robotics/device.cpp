#include <gygax/robotics/device.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>

#include <gygax/core/charconv.hpp>
#include <chrono>
#include <cmath>
#include <format>
#include <thread>

#include <gygax/bus/dbc.hpp>
#include <gygax/bus/j1939.hpp>
#include <gygax/bus/modbus.hpp>
#include <gygax/bus/obd2.hpp>
#include <gygax/net/http.hpp>
#include <gygax/net/link.hpp>
#include <gygax/robotics/adsb.hpp>
#include <gygax/robotics/mavlink.hpp>
#include <gygax/robotics/nmea.hpp>
#include <gygax/robotics/rosbridge.hpp>

namespace gygax::robotics {

using namespace std::chrono_literals;

namespace {

constexpr std::size_t kMaxDbcBytes = 1 << 20;
constexpr std::size_t kSerialUriSchemeLength = 9;
constexpr std::size_t kMaximumDeviceIdLength = 64;
constexpr std::size_t kMaximumRegisteredDevices = 64;
constexpr std::size_t kMaximumModbusRegisters = 256;
constexpr std::size_t kCommandLongParameterCount = 7;
constexpr std::int64_t kMaximumObdEcuAddress = 7;
constexpr std::int64_t kMaximumModbusRegisterAddress = 65535;
constexpr std::int64_t kMaximumModbusUnitId = 247;
constexpr std::int64_t kMinimumObdPollIntervalMs = 100;
constexpr std::int64_t kMinimumModbusPollIntervalMs = 50;
constexpr std::int64_t kMaximumDevicePollIntervalMs = 60000;
constexpr double kMaximumFlightAltitudeMeters = 1000.0;
constexpr double kDefaultGotoAltitudeMeters = 10.0;
constexpr int kDefaultMavlinkParameterType = 9;
constexpr std::chrono::seconds kRosbridgeConnectTimeout{3};

std::string errorText(int rc) {
    return std::format("{} (errno {})", transport::describeError(rc), -rc);
}

std::string toHex(std::span<const std::uint8_t> bytes) {
    std::string out;
    for (const auto b : bytes) out += std::format("{:02X}", b);
    return out;
}

std::optional<std::vector<std::uint8_t>> fromHex(const std::string& text) {
    std::string clean;
    for (const char c : text) {
        if (c != ' ' && c != ':') clean.push_back(c);
    }
    if (clean.size() % 2 != 0) return std::nullopt;
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < clean.size(); i += 2) {
        unsigned v = 0;
        if (gygax::fromChars(clean.data() + i, clean.data() + i + 2, v, 16).ec != std::errc()) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>(v));
    }
    return out;
}

CommandResult failure(int status, std::string_view code, std::string_view message) {
    return CommandResult::error(status, code, message);
}

CommandResult fromRc(int rc, const char* what) {
    if (rc == 0) return CommandResult::ok();
    const int status = rc == -ETIMEDOUT ? net::kHttpStatusGatewayTimeout
                       : rc == -EINVAL  ? net::kHttpStatusBadRequest
                       : rc == -EPERM   ? net::kHttpStatusConflict
                                        : net::kHttpStatusBadGateway;
    return failure(status, "device_error", std::format("{} failed: {}", what, errorText(rc)));
}

std::shared_ptr<net::ByteLink> openChecked(const std::string& uri, std::string* error) {
    if (uri.starts_with("serial://")) {
        const auto path = uri.substr(kSerialUriSchemeLength,
                                     uri.find('?') == std::string::npos ? std::string::npos : uri.find('?') - kSerialUriSchemeLength);
        if (!serialPathAllowed(path)) {
            if (error != nullptr) *error = "serial devices must live under /dev/";
            return nullptr;
        }
    }
    return net::openLink(uri, error);
}

class MavlinkDevice final : public Device {
public:
    MavlinkDevice(std::shared_ptr<net::ByteLink> link, mavlink::VehicleOptions options, std::string endpoint)
        : vehicle_(std::move(link), options), endpoint_(std::move(endpoint)) {
        vehicle_.start();
    }

    std::string kind() const override { return "mavlink"; }
    std::string endpoint() const override { return endpoint_; }
    bool connected() const override { return vehicle_.state().connected; }
    json::Value state() override { return vehicle_.state().toJson(); }

    std::vector<std::string> commands() const override {
        return {"arm",  "disarm",    "set_mode",  "takeoff",      "land",         "rtl",
                "goto", "param_get", "param_set", "request_rate", "command_long", "message"};
    }

    CommandResult command(const json::Value& r) override {
        const auto name = r.getString("command");
        if (!vehicle_.state().connected && name != "message")
            return failure(net::kHttpStatusConflict, "not_connected", "no heartbeat from the vehicle");
        if (name == "arm") return fromRc(vehicle_.arm(r.getBool("force")), "arm");
        if (name == "disarm") return fromRc(vehicle_.disarm(r.getBool("force")), "disarm");
        if (name == "land") return fromRc(vehicle_.land(), "land");
        if (name == "rtl") return fromRc(vehicle_.returnToLaunch(), "return to launch");
        if (name == "takeoff") {
            const double altitude = r.getDouble("altitude", -1.0);
            if (altitude <= 0.0 || altitude > kMaximumFlightAltitudeMeters)
                return failure(net::kHttpStatusBadRequest, "invalid_request", "altitude must be within (0, 1000] meters");
            return fromRc(vehicle_.takeoff(altitude), "takeoff");
        }
        if (name == "set_mode") {
            std::optional<std::uint32_t> mode;
            if (const auto* m = r.find("mode"); m != nullptr && m->isString())
                mode = mavlink::Vehicle::arducopterMode(m->asString());
            else if (const auto* c = r.find("custom_mode"); c != nullptr && c->isNumber() && c->asInt() >= 0)
                mode = static_cast<std::uint32_t>(c->asInt());
            if (!mode)
                return failure(net::kHttpStatusBadRequest, "invalid_request", "unknown mode; use an ArduCopter mode name or custom_mode");
            return fromRc(vehicle_.setMode(*mode), "set_mode");
        }
        if (name == "goto") {
            if (!r.contains("latitude") || !r.contains("longitude"))
                return failure(net::kHttpStatusBadRequest, "invalid_request", "latitude and longitude are required");
            const double altitude = r.getDouble("altitude", kDefaultGotoAltitudeMeters);
            if (altitude < 0.0 || altitude > kMaximumFlightAltitudeMeters)
                return failure(net::kHttpStatusBadRequest, "invalid_request", "altitude must be within [0, 1000] meters");
            return fromRc(vehicle_.gotoGlobal(r.getDouble("latitude"), r.getDouble("longitude"), altitude), "goto");
        }
        if (name == "param_get") {
            double value = 0.0;
            if (const auto rc = vehicle_.readParameter(r.getString("name"), value); rc != 0) return fromRc(rc, "param_get");
            json::Value out = json::Value::object();
            out["name"] = r.getString("name");
            out["value"] = value;
            return CommandResult::ok(std::move(out));
        }
        if (name == "param_set") {
            if (!r.contains("value")) return failure(net::kHttpStatusBadRequest, "invalid_request", "value is required");
            return fromRc(vehicle_.setParameter(r.getString("name"), r.getDouble("value"),
                                                static_cast<std::uint8_t>(r.getInt("type", kDefaultMavlinkParameterType))),
                          "param_set");
        }
        if (name == "request_rate") {
            const auto id = r.getInt("message_id", -1);
            if (id < 0) return failure(net::kHttpStatusBadRequest, "invalid_request", "message_id is required");
            return fromRc(vehicle_.requestMessageRate(static_cast<std::uint32_t>(id), r.getDouble("hz", 1.0)), "request_rate");
        }
        if (name == "command_long") {
            const auto id = r.getInt("id", -1);
            if (id < 0 || id > 65535) return failure(net::kHttpStatusBadRequest, "invalid_request", "id must be a MAV_CMD number");
            std::array<double, 7> p{};
            if (const auto* params = r.find("params")) {
                for (std::size_t i = 0; i < kCommandLongParameterCount && i < params->asArray().size(); ++i)
                    p[i] = params->asArray()[i].asDouble();
            }
            std::uint8_t result = 0;
            const auto rc = vehicle_.sendCommand(static_cast<std::uint16_t>(id), p, &result);
            json::Value out = json::Value::object();
            out["result"] = result;
            if (rc == 0 || rc == -EPERM) return CommandResult::ok(std::move(out));
            return fromRc(rc, "command_long");
        }
        if (name == "message") {
            const auto* fields = r.find("fields");
            return fromRc(vehicle_.sendMessage(r.getString("name"), fields != nullptr ? *fields : json::Value::object()), "message");
        }
        return failure(net::kHttpStatusBadRequest, "unknown_command", "unknown mavlink command '" + name + "'");
    }

private:
    mavlink::Vehicle vehicle_;
    std::string endpoint_;
};

class CanDevice final : public Device {
public:
    CanDevice(std::shared_ptr<bus::CanBus> canBus, std::optional<bus::DbcDatabase> db, bool j1939, std::string endpoint)
        : bus_(std::move(canBus)), db_(std::move(db)), j1939_(j1939), endpoint_(std::move(endpoint)) {
        thread_ = std::jthread([this](const std::stop_token& st) { loop(st); });
    }

    std::string kind() const override { return "can"; }
    std::string endpoint() const override { return endpoint_; }
    bool connected() const override { return true; }

    json::Value state() override {
        std::lock_guard lock(mutex_);
        json::Value out = json::Value::object();
        out["interface"] = endpoint_;
        out["frames_rx"] = rx_;
        out["frames_tx"] = tx_;
        json::Value list = json::Value::array();
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [key, entry] : frames_) {
            json::Value item = json::Value::object();
            item["id"] = std::format("0x{:X}", entry.frame.id);
            item["extended"] = entry.frame.extended;
            item["count"] = entry.count;
            item["data"] = toHex(entry.frame.payload());
            item["age_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.seen).count();
            if (!entry.name.empty()) item["name"] = entry.name;
            if (entry.signals.isObject() && entry.signals.size() > 0) item["signals"] = entry.signals;
            list.push(std::move(item));
        }
        out["messages"] = std::move(list);
        return out;
    }

    std::vector<std::string> commands() const override { return {"send", "encode", "request_pgn"}; }

    CommandResult command(const json::Value& r) override {
        const auto name = r.getString("command");
        bus::CanFrame frame;
        if (name == "send") {
            const auto id = r.getInt("id", -1);
            const bool extended = r.getBool("extended", id > 0x7FF);
            auto data = fromHex(r.getString("data"));
            if (id < 0 || id > (extended ? 0x1FFFFFFF : 0x7FF) || !data)
                return failure(400, "invalid_request", "id and hex data are required");
            if (data->size() > (r.getBool("fd") ? 64U : 8U)) return failure(400, "invalid_request", "payload too long");
            frame = bus::CanFrame::make(static_cast<std::uint32_t>(id), *data, extended, r.getBool("fd"));
        } else if (name == "encode") {
            if (!db_) return failure(409, "no_dbc", "this device was opened without a DBC database");
            std::map<std::string, double> values;
            if (const auto* v = r.find("values")) {
                for (const auto& [k, item] : v->asObject()) values[k] = item.asDouble();
            }
            std::string error;
            auto encoded = db_->encode(r.getString("message"), values, &error);
            if (!encoded) return failure(400, "invalid_request", error);
            frame = *encoded;
        } else if (name == "request_pgn") {
            const auto pgn = r.getInt("pgn", -1);
            if (pgn < 0 || pgn > 0x3FFFF) return failure(400, "invalid_request", "pgn is required");
            frame = bus::j1939::makeRequest(static_cast<std::uint32_t>(pgn), static_cast<std::uint8_t>(r.getInt("destination", 0xFF)),
                                            static_cast<std::uint8_t>(r.getInt("source", 0xFE)));
        } else {
            return failure(400, "unknown_command", "unknown can command '" + name + "'");
        }
        const auto rc = bus_->send(frame);
        if (rc == 0) {
            std::lock_guard lock(mutex_);
            ++tx_;
        }
        return fromRc(rc, "send");
    }

private:
    struct Entry {
        bus::CanFrame frame;
        std::uint64_t count = 0;
        std::chrono::steady_clock::time_point seen;
        std::string name;
        json::Value signals = json::Value::object();
    };

    void loop(const std::stop_token& st) {
        while (!st.stop_requested()) {
            bus::CanFrame f;
            if (bus_->receive(f, 50ms) != 0) continue;
            std::lock_guard lock(mutex_);
            ++rx_;
            auto& e = frames_[{f.id, f.extended}];
            e.frame = f;
            ++e.count;
            e.seen = std::chrono::steady_clock::now();
            e.signals = json::Value::object();
            if (db_) {
                if (const auto* msg = db_->byId(f.id, f.extended)) {
                    e.name = msg->name;
                    for (const auto& s : db_->decode(f)) {
                        json::Value item = json::Value::object();
                        item["value"] = s.value;
                        if (!s.unit.empty()) item["unit"] = s.unit;
                        if (!s.choice.empty()) item["choice"] = s.choice;
                        e.signals[s.name] = std::move(item);
                    }
                }
            }
            if (j1939_ && f.extended) {
                const auto id = bus::j1939::Id::decode(f.id);
                e.name = e.name.empty() ? std::format("PGN {}", id.pgn) : e.name;
                for (const auto& s : bus::j1939::decode(id.pgn, f.payload())) {
                    json::Value item = json::Value::object();
                    item["value"] = s.value;
                    item["unit"] = s.unit;
                    e.signals[s.name] = std::move(item);
                }
            }
        }
    }

    std::shared_ptr<bus::CanBus> bus_;
    std::optional<bus::DbcDatabase> db_;
    bool j1939_;
    std::string endpoint_;
    std::mutex mutex_;
    std::map<std::pair<std::uint32_t, bool>, Entry> frames_;
    std::uint64_t rx_ = 0;
    std::uint64_t tx_ = 0;
    std::jthread thread_;
};

class ObdDevice final : public Device {
public:
    ObdDevice(std::shared_ptr<bus::CanBus> canBus, std::uint8_t ecu, std::chrono::milliseconds poll, std::string endpoint)
        : client_(std::move(canBus), ecu, 250ms), endpoint_(std::move(endpoint)) {
        thread_ = std::jthread([this, poll](const std::stop_token& st) {
            static constexpr std::uint8_t kPolledPids[] = {0x0C, 0x0D, 0x05, 0x11, 0x2F, 0x42, 0x10};
            while (!st.stop_requested()) {
                bool answered = false;
                for (const std::uint8_t pid : kPolledPids) {
                    if (st.stop_requested()) return;
                    std::optional<bus::Obd2Value> v;
                    {
                        std::lock_guard lock(clientMutex_);
                        v = client_.readPid(pid);
                    }
                    if (v) {
                        answered = true;
                        std::lock_guard sl(mutex_);
                        values_[v->name] = {v->value, v->unit};
                        lastOk_ = std::chrono::steady_clock::now();
                    } else if (!answered) {
                        break;
                    }
                }
                for (auto slept = 0ms; slept < poll && !st.stop_requested(); slept += 20ms) std::this_thread::sleep_for(20ms);
            }
        });
    }

    std::string kind() const override { return "obd"; }
    std::string endpoint() const override { return endpoint_; }

    bool connected() const override {
        std::lock_guard lock(mutex_);
        return lastOk_ != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() - lastOk_ < 10s;
    }

    json::Value state() override {
        std::lock_guard lock(mutex_);
        json::Value out = json::Value::object();
        for (const auto& [name, v] : values_) {
            out["values"][name]["value"] = v.first;
            out["values"][name]["unit"] = v.second;
        }
        out["connected"] = lastOk_ != std::chrono::steady_clock::time_point{};
        return out;
    }

    std::vector<std::string> commands() const override { return {"read_pid", "supported", "dtcs", "vin"}; }

    CommandResult command(const json::Value& r) override {
        const auto name = r.getString("command");
        std::lock_guard lock(clientMutex_);
        json::Value out = json::Value::object();
        if (name == "read_pid") {
            const auto pid = r.getInt("pid", -1);
            if (pid < 0 || pid > 255) return failure(400, "invalid_request", "pid is required");
            auto v = client_.readPid(static_cast<std::uint8_t>(pid));
            if (!v) return failure(504, "no_response", "the ECU did not answer for this PID");
            out["name"] = v->name;
            out["value"] = v->value;
            out["unit"] = v->unit;
            return CommandResult::ok(std::move(out));
        }
        if (name == "supported") {
            json::Value list = json::Value::array();
            for (const auto pid : client_.supportedPids()) list.push(pid);
            out["pids"] = std::move(list);
            return CommandResult::ok(std::move(out));
        }
        if (name == "dtcs") {
            json::Value list = json::Value::array();
            for (const auto& code : client_.readStoredDtcs()) list.push(code);
            out["codes"] = std::move(list);
            return CommandResult::ok(std::move(out));
        }
        if (name == "vin") {
            out["vin"] = client_.readVin();
            return CommandResult::ok(std::move(out));
        }
        return failure(400, "unknown_command", "unknown obd command '" + name + "'");
    }

private:
    bus::Obd2Client client_;
    std::string endpoint_;
    mutable std::mutex mutex_;
    std::mutex clientMutex_;
    std::map<std::string, std::pair<double, std::string>> values_;
    std::chrono::steady_clock::time_point lastOk_{};
    std::jthread thread_;
};

struct ModbusRegister {
    std::string name;
    std::string area = "holding";
    std::string type = "u16";
    std::uint16_t address = 0;
    double scale = 1.0;
    double offset = 0.0;
};

class ModbusDevice final : public Device {
public:
    ModbusDevice(std::shared_ptr<net::ByteLink> link, bus::modbus::Mode mode, std::uint8_t unit, std::vector<ModbusRegister> regs,
                 std::chrono::milliseconds poll, std::string endpoint)
        : client_(std::move(link), mode, 1000ms), unit_(unit), registers_(std::move(regs)), endpoint_(std::move(endpoint)) {
        thread_ = std::jthread([this, poll](const std::stop_token& st) {
            while (!st.stop_requested()) {
                pollOnce();
                for (auto slept = 0ms; slept < poll && !st.stop_requested(); slept += 20ms) std::this_thread::sleep_for(20ms);
            }
        });
    }

    std::string kind() const override { return "modbus"; }
    std::string endpoint() const override { return endpoint_; }

    bool connected() const override {
        std::lock_guard lock(mutex_);
        return healthy_;
    }

    json::Value state() override {
        std::lock_guard lock(mutex_);
        json::Value out = json::Value::object();
        out["values"] = values_;
        out["errors"] = errors_;
        out["polls"] = polls_;
        out["connected"] = healthy_;
        return out;
    }

    std::vector<std::string> commands() const override { return {"read", "write_register", "write_registers", "write_coil"}; }

    CommandResult command(const json::Value& r) override {
        const auto name = r.getString("command");
        std::lock_guard lock(clientMutex_);
        const auto address = r.getInt("address", -1);
        if (address < 0 || address > 65535) return failure(400, "invalid_request", "address is required");
        const auto addr = static_cast<std::uint16_t>(address);
        if (name == "read") {
            const auto area = r.getString("area", "holding");
            const auto count = static_cast<std::uint16_t>(std::clamp<std::int64_t>(r.getInt("count", 1), 1, 125));
            json::Value out = json::Value::object();
            json::Value list = json::Value::array();
            if (area == "coil" || area == "discrete") {
                std::vector<bool> bits;
                const auto rc =
                    area == "coil" ? client_.readCoils(unit_, addr, count, bits) : client_.readDiscreteInputs(unit_, addr, count, bits);
                if (rc != 0) return modbusFailure(rc);
                for (const bool b : bits) list.push(b);
            } else if (area == "holding" || area == "input") {
                std::vector<std::uint16_t> words;
                const auto rc = area == "holding" ? client_.readHoldingRegisters(unit_, addr, count, words)
                                                  : client_.readInputRegisters(unit_, addr, count, words);
                if (rc != 0) return modbusFailure(rc);
                for (const auto w : words) list.push(w);
            } else {
                return failure(400, "invalid_request", "area must be holding, input, coil or discrete");
            }
            out["values"] = std::move(list);
            return CommandResult::ok(std::move(out));
        }
        if (name == "write_register") {
            const auto value = r.getInt("value", -1);
            if (value < 0 || value > 65535) return failure(400, "invalid_request", "value must be within 0..65535");
            return modbusOr(client_.writeRegister(unit_, addr, static_cast<std::uint16_t>(value)));
        }
        if (name == "write_registers") {
            std::vector<std::uint16_t> values;
            if (const auto* v = r.find("values")) {
                for (const auto& item : v->asArray()) {
                    if (item.asInt(-1) < 0 || item.asInt() > 65535)
                        return failure(400, "invalid_request", "values must be within 0..65535");
                    values.push_back(static_cast<std::uint16_t>(item.asInt()));
                }
            }
            if (values.empty()) return failure(400, "invalid_request", "values are required");
            return modbusOr(client_.writeRegisters(unit_, addr, values));
        }
        if (name == "write_coil") return modbusOr(client_.writeCoil(unit_, addr, r.getBool("value")));
        return failure(400, "unknown_command", "unknown modbus command '" + name + "'");
    }

private:
    CommandResult modbusFailure(int rc) {
        if (rc == -EREMOTEIO)
            return failure(502, "modbus_exception",
                           std::format("device answered with exception {} ({})", client_.lastException(),
                                       bus::modbus::exceptionText(client_.lastException())));
        return fromRc(rc, "modbus request");
    }

    CommandResult modbusOr(int rc) { return rc == 0 ? CommandResult::ok() : modbusFailure(rc); }

    void pollOnce() {
        std::lock_guard cl(clientMutex_);
        bool ok = !registers_.empty();
        std::uint64_t failures = 0;
        json::Value values = json::Value::object();
        for (const auto& reg : registers_) {
            const bool wide = reg.type == "u32" || reg.type == "i32" || reg.type == "f32";
            const std::uint16_t count = wide ? 2 : 1;
            double value = 0.0;
            if (reg.area == "coil" || reg.area == "discrete") {
                std::vector<bool> bits;
                const auto rc = reg.area == "coil" ? client_.readCoils(unit_, reg.address, 1, bits)
                                                   : client_.readDiscreteInputs(unit_, reg.address, 1, bits);
                if (rc != 0) {
                    ++failures;
                    continue;
                }
                value = bits[0] ? 1.0 : 0.0;
            } else {
                std::vector<std::uint16_t> words;
                const auto rc = reg.area == "input" ? client_.readInputRegisters(unit_, reg.address, count, words)
                                                    : client_.readHoldingRegisters(unit_, reg.address, count, words);
                if (rc != 0) {
                    ++failures;
                    continue;
                }
                if (reg.type == "i16")
                    value = static_cast<std::int16_t>(words[0]);
                else if (reg.type == "u32")
                    value = static_cast<double>((static_cast<std::uint32_t>(words[0]) << 16) | words[1]);
                else if (reg.type == "i32")
                    value = static_cast<double>(static_cast<std::int32_t>((static_cast<std::uint32_t>(words[0]) << 16) | words[1]));
                else if (reg.type == "f32")
                    value = bus::modbus::Client::registersToFloat(words[0], words[1]);
                else
                    value = words[0];
            }
            values[reg.name] = value * reg.scale + reg.offset;
        }
        ok = ok && failures == 0;
        std::lock_guard lock(mutex_);
        ++polls_;
        errors_ += failures;
        healthy_ = ok || (registers_.empty() && polls_ > 0);
        if (values.size() > 0) values_ = std::move(values);
    }

    bus::modbus::Client client_;
    std::uint8_t unit_;
    std::vector<ModbusRegister> registers_;
    std::string endpoint_;
    mutable std::mutex mutex_;
    std::mutex clientMutex_;
    json::Value values_ = json::Value::object();
    std::uint64_t errors_ = 0;
    std::uint64_t polls_ = 0;
    bool healthy_ = false;
    std::jthread thread_;
};

class NmeaDevice final : public Device {
public:
    NmeaDevice(std::shared_ptr<net::ByteLink> link, std::string endpoint) : link_(std::move(link)), endpoint_(std::move(endpoint)) {
        thread_ = std::jthread([this](const std::stop_token& st) {
            std::vector<std::uint8_t> chunk;
            while (!st.stop_requested()) {
                if (link_->read(chunk, 100ms) != 0) continue;
                const auto lines = assembler_.feed(std::string_view(reinterpret_cast<const char*>(chunk.data()), chunk.size()));
                std::lock_guard lock(mutex_);
                for (const auto& line : lines) {
                    auto s = nmea::parse(line);
                    if (!s) {
                        ++invalid_;
                        continue;
                    }
                    if (s->checksumPresent && !s->checksumValid) {
                        ++invalid_;
                        continue;
                    }
                    ++sentences_;
                    lastSeen_ = std::chrono::steady_clock::now();
                    state_.update(*s);
                }
            }
        });
    }

    std::string kind() const override { return "nmea"; }
    std::string endpoint() const override { return endpoint_; }

    bool connected() const override {
        std::lock_guard lock(mutex_);
        return lastSeen_ != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() - lastSeen_ < 10s;
    }

    json::Value state() override {
        std::lock_guard lock(mutex_);
        auto out = state_.toJson();
        out["sentences"] = sentences_;
        out["invalid_sentences"] = invalid_;
        return out;
    }

    std::vector<std::string> commands() const override { return {}; }
    CommandResult command(const json::Value&) override { return failure(400, "unsupported", "NMEA devices are read-only"); }

private:
    std::shared_ptr<net::ByteLink> link_;
    std::string endpoint_;
    mutable std::mutex mutex_;
    nmea::LineAssembler assembler_;
    nmea::State state_;
    std::uint64_t sentences_ = 0;
    std::uint64_t invalid_ = 0;
    std::chrono::steady_clock::time_point lastSeen_{};
    std::jthread thread_;
};

class AdsbDevice final : public Device {
public:
    AdsbDevice(std::shared_ptr<net::ByteLink> link, bool sbs, std::optional<std::pair<double, double>> reference, std::string endpoint)
        : link_(std::move(link)), sbs_(sbs), endpoint_(std::move(endpoint)) {
        if (reference) tracker_.setReference(reference->first, reference->second);
        thread_ = std::jthread([this](const std::stop_token& st) {
            std::vector<std::uint8_t> chunk;
            while (!st.stop_requested()) {
                if (link_->read(chunk, 100ms) != 0) continue;
                std::lock_guard lock(mutex_);
                const std::string_view text(reinterpret_cast<const char*>(chunk.data()), chunk.size());
                if (sbs_) {
                    sbsBuffer_.append(text);
                    std::size_t start = 0;
                    while (true) {
                        const auto nl = sbsBuffer_.find('\n', start);
                        if (nl == std::string::npos) break;
                        if (tracker_.ingestSbs(std::string_view(sbsBuffer_).substr(start, nl - start)))
                            lastSeen_ = std::chrono::steady_clock::now();
                        start = nl + 1;
                    }
                    sbsBuffer_.erase(0, start);
                    if (sbsBuffer_.size() > 4096) sbsBuffer_.clear();
                } else if (tracker_.ingestAvrStream(text) > 0) {
                    lastSeen_ = std::chrono::steady_clock::now();
                }
                tracker_.expire(120s);
            }
        });
    }

    std::string kind() const override { return "adsb"; }
    std::string endpoint() const override { return endpoint_; }

    bool connected() const override {
        std::lock_guard lock(mutex_);
        return lastSeen_ != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() - lastSeen_ < 30s;
    }

    json::Value state() override {
        std::lock_guard lock(mutex_);
        return tracker_.toJson();
    }

    std::vector<std::string> commands() const override { return {}; }
    CommandResult command(const json::Value&) override { return failure(400, "unsupported", "ADS-B devices are receive-only"); }

private:
    std::shared_ptr<net::ByteLink> link_;
    bool sbs_;
    std::string endpoint_;
    mutable std::mutex mutex_;
    adsb::Tracker tracker_;
    std::string sbsBuffer_;
    std::chrono::steady_clock::time_point lastSeen_{};
    std::jthread thread_;
};

class RosDevice final : public Device {
public:
    RosDevice(std::unique_ptr<RosbridgeClient> client, std::string endpoint) : client_(std::move(client)), endpoint_(std::move(endpoint)) {
        client_->start();
    }

    std::string kind() const override { return "rosbridge"; }
    std::string endpoint() const override { return endpoint_; }
    bool connected() const override { return client_->connected(); }

    json::Value state() override {
        json::Value out = json::Value::object();
        out["connected"] = client_->connected();
        for (const auto& topic : client_->subscribedTopics()) {
            if (auto m = client_->lastMessage(topic))
                out["topics"][topic] = *m;
            else
                out["topics"][topic] = nullptr;
        }
        return out;
    }

    std::vector<std::string> commands() const override { return {"subscribe", "unsubscribe", "publish", "twist", "call_service"}; }

    CommandResult command(const json::Value& r) override {
        const auto name = r.getString("command");
        if (name == "subscribe")
            return fromRc(
                client_->subscribe(r.getString("topic"), r.getString("type"), nullptr, static_cast<int>(r.getInt("throttle_ms", 0))),
                "subscribe");
        if (name == "unsubscribe") return fromRc(client_->unsubscribe(r.getString("topic")), "unsubscribe");
        if (name == "publish") {
            const auto topic = r.getString("topic");
            if (const auto type = r.getString("type"); !type.empty()) {
                if (const auto rc = client_->advertise(topic, type); rc != 0) return fromRc(rc, "advertise");
            }
            const auto* msg = r.find("msg");
            return fromRc(client_->publish(topic, msg != nullptr ? *msg : json::Value()), "publish");
        }
        if (name == "twist") {
            const double linear = r.getDouble("linear");
            const double angular = r.getDouble("angular");
            if (std::fabs(linear) > 5.0 || std::fabs(angular) > 6.3)
                return failure(400, "invalid_request", "twist exceeds safe limits (5 m/s, 6.3 rad/s)");
            const auto topic = r.getString("topic", "/cmd_vel");
            (void)client_->advertise(topic, "geometry_msgs/Twist");
            return fromRc(client_->publishTwist(topic, linear, angular), "twist");
        }
        if (name == "call_service") {
            json::Value result;
            const auto* args = r.find("args");
            const auto rc = client_->callService(r.getString("service"), args != nullptr ? *args : json::Value::object(), result);
            if (rc != 0) return fromRc(rc, "call_service");
            json::Value out = json::Value::object();
            out["values"] = std::move(result);
            return CommandResult::ok(std::move(out));
        }
        return failure(400, "unknown_command", "unknown rosbridge command '" + name + "'");
    }

private:
    std::unique_ptr<RosbridgeClient> client_;
    std::string endpoint_;
};

std::shared_ptr<bus::CanBus> openCanBus(const std::string& iface, bool fd, std::string* error) {
    if (iface.starts_with("virtual:")) return virtualCanNetwork(iface.substr(8))->attach("gygax");
    return std::shared_ptr<bus::CanBus>(bus::SocketCanBus::open(iface, fd, error));
}

std::optional<bus::DbcDatabase> loadDbc(const json::Value& config, std::string* error) {
    const auto text = config.getString("dbc");
    if (text.empty()) return std::nullopt;
    if (text.size() > kMaxDbcBytes) {
        if (error != nullptr) *error = "dbc is larger than 1 MiB";
        return std::nullopt;
    }
    std::string parseError;
    auto db = bus::DbcDatabase::parse(text, &parseError);
    if (!db && error != nullptr) *error = "invalid dbc: " + parseError;
    return db;
}

} // namespace

CommandResult CommandResult::ok(json::Value body) {
    CommandResult r;
    r.status = 200;
    r.body = std::move(body);
    return r;
}

CommandResult CommandResult::error(int status, std::string_view code, std::string_view message) {
    CommandResult r;
    r.status = status;
    r.body = json::Value::object();
    r.body["error"]["code"] = std::string(code);
    r.body["error"]["message"] = std::string(message);
    r.body["error"]["status"] = status;
    return r;
}

std::shared_ptr<bus::LoopbackCanNetwork> virtualCanNetwork(const std::string& name) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<bus::LoopbackCanNetwork>> networks;
    std::lock_guard lock(mutex);
    auto& slot = networks[name];
    if (!slot) slot = std::make_shared<bus::LoopbackCanNetwork>();
    return slot;
}

bool serialPathAllowed(const std::string& path) {
    return path.starts_with("/dev/") && path.find("..") == std::string::npos;
}

std::vector<std::string> deviceKinds() {
    return {"mavlink", "can", "obd", "modbus", "nmea", "adsb", "rosbridge"};
}

std::unique_ptr<Device> openDevice(const json::Value& config, std::string* error) {
    auto fail = [&](std::string message) -> std::unique_ptr<Device> {
        if (error != nullptr) *error = std::move(message);
        return nullptr;
    };
    if (!config.isObject()) return fail("device config must be an object");
    const auto kind = config.getString("kind");
    const auto uri = config.getString("uri");
    std::string err;

    if (kind == "mavlink") {
        auto link = openChecked(uri, &err);
        if (!link) return fail(err.empty() ? "a uri is required (udp://, tcp:// or serial://)" : err);
        mavlink::VehicleOptions options;
        options.targetSystem = static_cast<std::uint8_t>(config.getInt("target_system", 1));
        options.targetComponent = static_cast<std::uint8_t>(config.getInt("target_component", 1));
        options.ourSystemId = static_cast<std::uint8_t>(config.getInt("system_id", 255));
        return std::make_unique<MavlinkDevice>(std::move(link), options, uri);
    }
    if (kind == "can" || kind == "obd") {
        const auto iface = config.getString("interface");
        if (iface.empty()) return fail("an interface is required (for example can0 or virtual:bench)");
        auto canBus = openCanBus(iface, config.getBool("fd"), &err);
        if (!canBus) return fail(err.empty() ? "cannot open the CAN interface" : err);
        if (kind == "obd") {
            return std::make_unique<ObdDevice>(
                std::move(canBus), static_cast<std::uint8_t>(std::clamp<std::int64_t>(config.getInt("ecu", 0), 0, kMaximumObdEcuAddress)),
                std::chrono::milliseconds(
                    std::clamp<std::int64_t>(config.getInt("poll_ms", 1000), kMinimumObdPollIntervalMs, kMaximumDevicePollIntervalMs)),
                iface);
        }
        auto db = loadDbc(config, &err);
        if (!err.empty()) return fail(err);
        return std::make_unique<CanDevice>(std::move(canBus), std::move(db), config.getBool("j1939"), iface);
    }
    if (kind == "modbus") {
        auto link = openChecked(uri, &err);
        if (!link) return fail(err.empty() ? "a uri is required (tcp:// or serial://)" : err);
        const auto modeName = config.getString("mode", uri.starts_with("tcp://") ? "tcp" : "rtu");
        if (modeName != "tcp" && modeName != "rtu") return fail("mode must be tcp or rtu");
        std::vector<ModbusRegister> regs;
        if (const auto* list = config.find("registers")) {
            for (const auto& item : list->asArray()) {
                ModbusRegister reg;
                reg.name = item.getString("name");
                reg.area = item.getString("area", "holding");
                reg.type = item.getString("type", "u16");
                reg.address =
                    static_cast<std::uint16_t>(std::clamp<std::int64_t>(item.getInt("address", 0), 0, kMaximumModbusRegisterAddress));
                reg.scale = item.getDouble("scale", 1.0);
                reg.offset = item.getDouble("offset", 0.0);
                if (reg.name.empty()) return fail("every register needs a name");
                if (reg.area != "holding" && reg.area != "input" && reg.area != "coil" && reg.area != "discrete")
                    return fail("invalid register area '" + reg.area + "'");
                if (reg.type != "u16" && reg.type != "i16" && reg.type != "u32" && reg.type != "i32" && reg.type != "f32")
                    return fail("invalid register type '" + reg.type + "'");
                regs.push_back(std::move(reg));
            }
        }
        if (regs.size() > kMaximumModbusRegisters) return fail("too many registers (limit 256)");
        return std::make_unique<ModbusDevice>(
            std::move(link), modeName == "tcp" ? bus::modbus::Mode::Tcp : bus::modbus::Mode::Rtu,
            static_cast<std::uint8_t>(std::clamp<std::int64_t>(config.getInt("unit", 1), 0, kMaximumModbusUnitId)), std::move(regs),
            std::chrono::milliseconds(
                std::clamp<std::int64_t>(config.getInt("poll_ms", 1000), kMinimumModbusPollIntervalMs, kMaximumDevicePollIntervalMs)),
            uri);
    }
    if (kind == "nmea") {
        auto link = openChecked(uri, &err);
        if (!link) return fail(err.empty() ? "a uri is required" : err);
        return std::make_unique<NmeaDevice>(std::move(link), uri);
    }
    if (kind == "adsb") {
        auto link = openChecked(uri, &err);
        if (!link) return fail(err.empty() ? "a uri is required" : err);
        const auto format = config.getString("format", "avr");
        if (format != "avr" && format != "sbs") return fail("format must be avr or sbs");
        std::optional<std::pair<double, double>> ref;
        if (const auto* r = config.find("reference")) ref = std::pair<double, double>{r->getDouble("latitude"), r->getDouble("longitude")};
        return std::make_unique<AdsbDevice>(std::move(link), format == "sbs", ref, uri);
    }
    if (kind == "rosbridge") {
        auto client = RosbridgeClient::connect(uri, kRosbridgeConnectTimeout, &err);
        if (!client) return fail(err);
        if (const auto* topics = config.find("subscribe")) {
            for (const auto& t : topics->asArray()) {
                const auto topic = t.isString() ? t.asString() : t.getString("topic");
                const auto type = t.isString() ? std::string() : t.getString("type");
                if (!topic.empty()) (void)client->subscribe(topic, type);
            }
        }
        return std::make_unique<RosDevice>(std::move(client), uri);
    }
    return fail("unknown device kind '" + kind + "' (supported: mavlink, can, obd, modbus, nmea, adsb, rosbridge)");
}

bool DeviceRegistry::add(const std::string& id, std::unique_ptr<Device> device, std::string* error) {
    if (id.empty() || id.size() > kMaximumDeviceIdLength || !std::ranges::all_of(id, [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.';
        })) {
        if (error != nullptr) *error = "device id must be 1-64 characters of letters, digits, '-', '_' or '.'";
        return false;
    }
    std::lock_guard lock(mutex_);
    if (devices_.size() >= kMaximumRegisteredDevices) {
        if (error != nullptr) *error = "too many devices (limit 64)";
        return false;
    }
    if (devices_.contains(id)) {
        if (error != nullptr) *error = "device id already exists: " + id;
        return false;
    }
    devices_[id] = std::move(device);
    return true;
}

bool DeviceRegistry::remove(const std::string& id) {
    std::shared_ptr<Device> doomed;
    {
        std::lock_guard lock(mutex_);
        auto it = devices_.find(id);
        if (it == devices_.end()) return false;
        doomed = std::move(it->second);
        devices_.erase(it);
    }
    return true;
}

std::shared_ptr<Device> DeviceRegistry::get(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = devices_.find(id);
    return it == devices_.end() ? nullptr : it->second;
}

std::vector<std::string> DeviceRegistry::ids() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.reserve(devices_.size());
    for (const auto& [id, d] : devices_) out.push_back(id);
    return out;
}

json::Value DeviceRegistry::list() const {
    std::vector<std::pair<std::string, std::shared_ptr<Device>>> snapshot;
    {
        std::lock_guard lock(mutex_);
        for (const auto& kv : devices_) snapshot.push_back(kv);
    }
    json::Value out = json::Value::array();
    for (const auto& [id, d] : snapshot) {
        json::Value item = json::Value::object();
        item["id"] = id;
        item["kind"] = d->kind();
        item["endpoint"] = d->endpoint();
        item["connected"] = d->connected();
        json::Value cmds = json::Value::array();
        for (const auto& c : d->commands()) cmds.push(c);
        item["commands"] = std::move(cmds);
        out.push(std::move(item));
    }
    return out;
}

void DeviceRegistry::clear() {
    std::map<std::string, std::shared_ptr<Device>> doomed;
    {
        std::lock_guard lock(mutex_);
        doomed.swap(devices_);
    }
}

} // namespace gygax::robotics
