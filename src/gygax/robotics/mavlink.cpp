#include <gygax/robotics/mavlink.hpp>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace gygax::robotics::mavlink {

namespace {

const std::vector<MessageDef>& table() {
    static const std::vector<MessageDef> defs = {
#include "mavlink_messages.inc"
    };
    return defs;
}

const std::unordered_map<std::uint32_t, const MessageDef*>& byId() {
    static const auto map = [] {
        std::unordered_map<std::uint32_t, const MessageDef*> m;
        for (const auto& d : table()) m[d.id] = &d;
        return m;
    }();
    return map;
}

std::size_t typeSize(FieldType t) {
    switch (t) {
    case FieldType::U8:
    case FieldType::I8:
    case FieldType::CHAR: return 1;
    case FieldType::U16:
    case FieldType::I16: return 2;
    case FieldType::U32:
    case FieldType::I32:
    case FieldType::F32: return 4;
    default: return 8;
    }
}

std::uint64_t readLe(const std::uint8_t* p, std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

void writeLe(std::uint8_t* p, std::size_t n, std::uint64_t v) {
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

json::Value readScalar(FieldType t, const std::uint8_t* p) {
    const std::uint64_t raw = readLe(p, typeSize(t));
    switch (t) {
    case FieldType::U8: return json::Value(static_cast<std::int64_t>(static_cast<std::uint8_t>(raw)));
    case FieldType::I8: return json::Value(static_cast<std::int64_t>(static_cast<std::int8_t>(raw)));
    case FieldType::U16: return json::Value(static_cast<std::int64_t>(static_cast<std::uint16_t>(raw)));
    case FieldType::I16: return json::Value(static_cast<std::int64_t>(static_cast<std::int16_t>(raw)));
    case FieldType::U32: return json::Value(static_cast<std::int64_t>(static_cast<std::uint32_t>(raw)));
    case FieldType::I32: return json::Value(static_cast<std::int64_t>(static_cast<std::int32_t>(raw)));
    case FieldType::U64: return json::Value(static_cast<double>(raw));
    case FieldType::I64: return json::Value(static_cast<std::int64_t>(raw));
    case FieldType::F32: return json::Value(static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw))));
    case FieldType::F64: return json::Value(std::bit_cast<double>(raw));
    case FieldType::CHAR: return json::Value(static_cast<std::int64_t>(raw));
    }
    return {};
}

void writeScalar(FieldType t, std::uint8_t* p, double value) {
    const auto n = typeSize(t);
    switch (t) {
    case FieldType::F32: writeLe(p, n, std::bit_cast<std::uint32_t>(static_cast<float>(value))); break;
    case FieldType::F64: writeLe(p, n, std::bit_cast<std::uint64_t>(value)); break;
    case FieldType::U64: writeLe(p, n, value <= 0.0 ? 0 : static_cast<std::uint64_t>(value)); break;
    default: writeLe(p, n, static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(value)))); break;
    }
}

bool fitsType(FieldType t, double v) {
    if (!std::isfinite(v) && t != FieldType::F32 && t != FieldType::F64) return false;
    switch (t) {
    case FieldType::U8: return v >= 0 && v <= 255;
    case FieldType::I8: return v >= -128 && v <= 127;
    case FieldType::U16: return v >= 0 && v <= 65535;
    case FieldType::I16: return v >= -32768 && v <= 32767;
    case FieldType::U32: return v >= 0 && v <= 4294967295.0;
    case FieldType::I32: return v >= -2147483648.0 && v <= 2147483647.0;
    case FieldType::U64: return v >= 0;
    default: return true;
    }
}

} // namespace

const MessageDef* findMessage(std::uint32_t id) {
    const auto& m = byId();
    auto it = m.find(id);
    return it == m.end() ? nullptr : it->second;
}

const MessageDef* findMessage(std::string_view name) {
    for (const auto& d : table()) {
        if (name == d.name) return &d;
    }
    return nullptr;
}

std::size_t messageCount() {
    return table().size();
}

std::uint16_t crc16(std::span<const std::uint8_t> data, std::uint16_t seed) {
    std::uint16_t crc = seed;
    for (const std::uint8_t byte : data) {
        std::uint8_t tmp = static_cast<std::uint8_t>(byte ^ (crc & 0xFF));
        tmp = static_cast<std::uint8_t>(tmp ^ (tmp << 4));
        crc = static_cast<std::uint16_t>((crc >> 8) ^ (static_cast<std::uint16_t>(tmp) << 8) ^ (static_cast<std::uint16_t>(tmp) << 3) ^
                                         (tmp >> 4));
    }
    return crc;
}

json::Value decodePayload(const MessageDef& def, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> full(def.maxLength, 0);
    std::copy_n(payload.begin(), std::min<std::size_t>(payload.size(), full.size()), full.begin());
    json::Value out = json::Value::object();
    for (const auto& f : def.fields) {
        const std::uint8_t* p = full.data() + f.offset;
        if (f.type == FieldType::CHAR && f.arrayLength > 0) {
            std::string text(reinterpret_cast<const char*>(p), static_cast<std::size_t>(f.arrayLength));
            if (const auto nul = text.find('\0'); nul != std::string::npos) text.resize(nul);
            out[f.name] = std::move(text);
        } else if (f.arrayLength > 0) {
            json::Value arr = json::Value::array();
            for (int i = 0; i < f.arrayLength; ++i) arr.push(readScalar(f.type, p + static_cast<std::size_t>(i) * typeSize(f.type)));
            out[f.name] = std::move(arr);
        } else {
            out[f.name] = readScalar(f.type, p);
        }
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> encodePayload(const MessageDef& def, const json::Value& fields, std::string* error) {
    auto fail = [&](std::string message) -> std::optional<std::vector<std::uint8_t>> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };
    if (!fields.isObject()) return fail("fields must be an object");
    for (const auto& [key, value] : fields.asObject()) {
        bool known = false;
        for (const auto& f : def.fields) known = known || key == f.name;
        if (!known) return fail(std::string(def.name) + " has no field '" + key + "'");
    }
    std::vector<std::uint8_t> payload(def.maxLength, 0);
    for (const auto& f : def.fields) {
        const auto* v = fields.find(f.name);
        if (v == nullptr) continue;
        std::uint8_t* p = payload.data() + f.offset;
        if (f.type == FieldType::CHAR && f.arrayLength > 0) {
            if (!v->isString()) return fail(std::string(f.name) + " must be a string");
            const auto& text = v->asString();
            if (text.size() > static_cast<std::size_t>(f.arrayLength))
                return fail(std::string(f.name) + " is longer than " + std::to_string(f.arrayLength) + " characters");
            std::copy(text.begin(), text.end(), p);
        } else if (f.arrayLength > 0) {
            if (!v->isArray() || v->asArray().size() > static_cast<std::size_t>(f.arrayLength))
                return fail(std::string(f.name) + " must be an array of at most " + std::to_string(f.arrayLength));
            std::size_t i = 0;
            for (const auto& item : v->asArray()) {
                if (!item.isNumber() || !fitsType(f.type, item.asDouble())) return fail(std::string(f.name) + " element out of range");
                writeScalar(f.type, p + i * typeSize(f.type), item.asDouble());
                ++i;
            }
        } else {
            if (!v->isNumber()) return fail(std::string(f.name) + " must be a number");
            if (!fitsType(f.type, v->asDouble())) return fail(std::string(f.name) + " is out of range");
            writeScalar(f.type, p, v->asDouble());
        }
    }
    return payload;
}

std::vector<std::uint8_t> serialize(const Frame& frame, bool truncateZeros) {
    const MessageDef* def = findMessage(frame.messageId);
    std::vector<std::uint8_t> payload = frame.payload;
    if (frame.version == 2 && truncateZeros) {
        while (payload.size() > 1 && payload.back() == 0) payload.pop_back();
    }
    std::vector<std::uint8_t> out;
    if (frame.version == 1) {
        out = {0xFE,
               static_cast<std::uint8_t>(payload.size()),
               frame.sequence,
               frame.systemId,
               frame.componentId,
               static_cast<std::uint8_t>(frame.messageId)};
    } else {
        out = {0xFD,
               static_cast<std::uint8_t>(payload.size()),
               0,
               0,
               frame.sequence,
               frame.systemId,
               frame.componentId,
               static_cast<std::uint8_t>(frame.messageId & 0xFF),
               static_cast<std::uint8_t>((frame.messageId >> 8) & 0xFF),
               static_cast<std::uint8_t>((frame.messageId >> 16) & 0xFF)};
    }
    out.insert(out.end(), payload.begin(), payload.end());
    std::uint16_t crc = crc16({out.data() + 1, out.size() - 1});
    const std::uint8_t extra = def != nullptr ? def->crcExtra : 0;
    crc = crc16({&extra, 1}, crc);
    out.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    out.push_back(static_cast<std::uint8_t>(crc >> 8));
    return out;
}

std::vector<Frame> Parser::feed(std::span<const std::uint8_t> data) {
    stats_.bytes += data.size();
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    std::vector<Frame> frames;
    std::size_t pos = 0;
    while (pos < buffer_.size()) {
        const std::uint8_t stx = buffer_[pos];
        if (stx != 0xFD && stx != 0xFE) {
            ++pos;
            ++stats_.resyncs;
            continue;
        }
        const bool v2 = stx == 0xFD;
        const std::size_t headerLen = v2 ? 10 : 6;
        if (buffer_.size() - pos < headerLen) break;
        const std::size_t len = buffer_[pos + 1];
        Frame f;
        f.version = v2 ? 2 : 1;
        if (v2) {
            f.sequence = buffer_[pos + 4];
            f.systemId = buffer_[pos + 5];
            f.componentId = buffer_[pos + 6];
            f.messageId = buffer_[pos + 7] | (static_cast<std::uint32_t>(buffer_[pos + 8]) << 8) |
                          (static_cast<std::uint32_t>(buffer_[pos + 9]) << 16);
        } else {
            f.sequence = buffer_[pos + 2];
            f.systemId = buffer_[pos + 3];
            f.componentId = buffer_[pos + 4];
            f.messageId = buffer_[pos + 5];
        }
        const MessageDef* def = findMessage(f.messageId);
        if (def == nullptr) {
            ++stats_.unknownMessages;
            ++pos;
            ++stats_.resyncs;
            continue;
        }
        if (len > def->maxLength || (v2 && len == 0) || (!v2 && len != def->baseLength)) {
            ++pos;
            ++stats_.resyncs;
            continue;
        }
        const bool hasSignature = v2 && (buffer_[pos + 2] & 0x01) != 0;
        const std::size_t total = headerLen + len + 2 + (hasSignature ? 13 : 0);
        if (buffer_.size() - pos < total) break;
        f.signedFrame = hasSignature;
        std::uint16_t crc = crc16({buffer_.data() + pos + 1, headerLen - 1 + len});
        crc = crc16({&def->crcExtra, 1}, crc);
        const std::uint16_t received =
            static_cast<std::uint16_t>(buffer_[pos + headerLen + len] | (buffer_[pos + headerLen + len + 1] << 8));
        if (crc != received) {
            ++stats_.crcErrors;
            ++pos;
            ++stats_.resyncs;
            continue;
        }
        f.payload.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(pos + headerLen),
                         buffer_.begin() + static_cast<std::ptrdiff_t>(pos + headerLen + len));
        if (f.payload.size() < def->maxLength) f.payload.resize(def->maxLength, 0);
        frames.push_back(std::move(f));
        ++stats_.frames;
        pos += total;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos));
    if (buffer_.size() > 4096) buffer_.erase(buffer_.begin(), buffer_.end() - 512);
    return frames;
}

json::Value VehicleState::toJson() const {
    json::Value v = json::Value::object();
    v["connected"] = connected;
    v["system_id"] = systemId;
    v["component_id"] = componentId;
    v["vehicle_type"] = vehicleType;
    v["autopilot"] = autopilot;
    v["armed"] = armed;
    v["base_mode"] = baseMode;
    v["custom_mode"] = customMode;
    v["system_status"] = systemStatus;
    v["latitude"] = latitude;
    v["longitude"] = longitude;
    v["altitude_msl"] = altitudeMsl;
    v["altitude_relative"] = altitudeRelative;
    v["velocity"]["x"] = vx;
    v["velocity"]["y"] = vy;
    v["velocity"]["z"] = vz;
    v["heading_deg"] = headingDeg;
    v["attitude"]["roll"] = roll;
    v["attitude"]["pitch"] = pitch;
    v["attitude"]["yaw"] = yaw;
    v["groundspeed"] = groundspeed;
    v["airspeed"] = airspeed;
    v["climb"] = climb;
    v["battery"]["voltage"] = batteryVoltage;
    v["battery"]["current"] = batteryCurrent;
    v["battery"]["remaining_percent"] = batteryRemaining;
    v["gps"]["fix_type"] = gpsFix;
    v["gps"]["satellites"] = satellites;
    v["status_text"] = lastStatusText;
    v["heartbeats"] = heartbeats;
    v["messages"] = messages;
    return v;
}

Vehicle::Vehicle(std::shared_ptr<net::ByteLink> link, VehicleOptions options) : link_(std::move(link)), options_(options) {}

Vehicle::~Vehicle() {
    stop();
}

void Vehicle::start() {
    if (thread_.joinable()) return;
    thread_ = std::jthread([this](const std::stop_token& st) { loop(st); });
}

void Vehicle::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

IOResult Vehicle::writeFrame(std::uint32_t messageId, const std::vector<std::uint8_t>& payload) {
    Frame f;
    f.sequence = sequence_.fetch_add(1);
    f.systemId = options_.ourSystemId;
    f.componentId = options_.ourComponentId;
    f.messageId = messageId;
    f.payload = payload;
    const auto bytes = serialize(f);
    std::lock_guard lock(writeMutex_);
    return link_->write(bytes);
}

IOResult Vehicle::sendMessage(std::string_view name, const json::Value& fields) {
    const auto* def = findMessage(name);
    if (def == nullptr) return -ENOENT;
    auto payload = encodePayload(*def, fields);
    if (!payload) return -EINVAL;
    return writeFrame(def->id, *payload);
}

void Vehicle::loop(const std::stop_token& stop) {
    auto nextHeartbeat = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> chunk;
    while (!stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextHeartbeat) {
            json::Value hb = json::Value::object();
            hb["type"] = 6;
            hb["autopilot"] = 8;
            hb["system_status"] = 4;
            hb["mavlink_version"] = 3;
            (void)sendMessage("HEARTBEAT", hb);
            nextHeartbeat = now + options_.heartbeatInterval;
        }
        const auto rc = link_->read(chunk, std::chrono::milliseconds(50));
        if (rc == 0) {
            std::vector<Frame> frames;
            {
                std::lock_guard lock(mutex_);
                frames = parser_.feed(chunk);
            }
            for (const auto& f : frames) handle(f);
        } else if (rc != -ETIMEDOUT && rc != -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::lock_guard lock(mutex_);
        if (state_.connected && lastHeartbeat_ != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() - lastHeartbeat_ > options_.heartbeatTimeout) {
            state_.connected = false;
            changed_.notify_all();
        }
    }
}

void Vehicle::handle(const Frame& frame) {
    const MessageDef* def = findMessage(frame.messageId);
    if (def == nullptr) return;
    const json::Value fields = decodePayload(*def, frame.payload);
    const std::string name = def->name;
    std::function<void(const std::string&, const Frame&, const json::Value&)> handler;
    {
        std::lock_guard lock(mutex_);
        const bool fromTarget = state_.systemId == 0
                                    ? (!options_.followFirstVehicle || frame.systemId == options_.targetSystem || name == "HEARTBEAT")
                                    : frame.systemId == state_.systemId;
        if (!fromTarget || frame.systemId == options_.ourSystemId) return;
        ++state_.messages;
        last_[name] = fields;
        if (name == "HEARTBEAT") {
            if (fields.getInt("type") == 6 && fields.getInt("autopilot") == 8) return;
            if (state_.systemId == 0) {
                state_.systemId = frame.systemId;
                state_.componentId = frame.componentId;
            }
            if (frame.componentId != state_.componentId) return;
            state_.vehicleType = static_cast<std::uint8_t>(fields.getInt("type"));
            state_.autopilot = static_cast<std::uint8_t>(fields.getInt("autopilot"));
            state_.baseMode = static_cast<std::uint8_t>(fields.getInt("base_mode"));
            state_.customMode = static_cast<std::uint32_t>(fields.getInt("custom_mode"));
            state_.systemStatus = static_cast<std::uint8_t>(fields.getInt("system_status"));
            state_.armed = (state_.baseMode & 0x80) != 0;
            state_.connected = true;
            ++state_.heartbeats;
            lastHeartbeat_ = std::chrono::steady_clock::now();
        } else if (name == "GLOBAL_POSITION_INT") {
            state_.latitude = static_cast<double>(fields.getInt("lat")) / 1e7;
            state_.longitude = static_cast<double>(fields.getInt("lon")) / 1e7;
            state_.altitudeMsl = static_cast<double>(fields.getInt("alt")) / 1000.0;
            state_.altitudeRelative = static_cast<double>(fields.getInt("relative_alt")) / 1000.0;
            state_.vx = static_cast<double>(fields.getInt("vx")) / 100.0;
            state_.vy = static_cast<double>(fields.getInt("vy")) / 100.0;
            state_.vz = static_cast<double>(fields.getInt("vz")) / 100.0;
            const auto hdg = fields.getInt("hdg");
            if (hdg != 65535) state_.headingDeg = static_cast<double>(hdg) / 100.0;
        } else if (name == "ATTITUDE") {
            state_.roll = fields.getDouble("roll");
            state_.pitch = fields.getDouble("pitch");
            state_.yaw = fields.getDouble("yaw");
        } else if (name == "VFR_HUD") {
            state_.groundspeed = fields.getDouble("groundspeed");
            state_.airspeed = fields.getDouble("airspeed");
            state_.climb = fields.getDouble("climb");
        } else if (name == "SYS_STATUS") {
            state_.batteryVoltage = static_cast<double>(fields.getInt("voltage_battery")) / 1000.0;
            state_.batteryCurrent = static_cast<double>(fields.getInt("current_battery")) / 100.0;
            state_.batteryRemaining = static_cast<int>(fields.getInt("battery_remaining"));
        } else if (name == "GPS_RAW_INT") {
            state_.gpsFix = static_cast<int>(fields.getInt("fix_type"));
            state_.satellites = static_cast<int>(fields.getInt("satellites_visible"));
        } else if (name == "STATUSTEXT") {
            state_.lastStatusText = fields.getString("text");
        } else if (name == "COMMAND_ACK") {
            ack_ = {static_cast<std::uint16_t>(fields.getInt("command")), static_cast<std::uint8_t>(fields.getInt("result"))};
        } else if (name == "PARAM_VALUE") {
            param_ = {fields.getString("param_id"), fields.getDouble("param_value")};
        }
        handler = handler_;
        changed_.notify_all();
    }
    if (handler) handler(name, frame, fields);
}

VehicleState Vehicle::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

bool Vehicle::waitForHeartbeat(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return state_.connected; });
}

IOResult Vehicle::sendCommand(std::uint16_t command, const std::array<double, 7>& params, std::uint8_t* result) {
    {
        std::lock_guard lock(mutex_);
        ack_.reset();
    }
    json::Value f = json::Value::object();
    for (std::size_t i = 0; i < params.size(); ++i) f["param" + std::to_string(i + 1)] = params[i];
    f["command"] = command;
    f["target_system"] = options_.targetSystem;
    f["target_component"] = options_.targetComponent;
    f["confirmation"] = 0;
    if (const auto rc = sendMessage("COMMAND_LONG", f); rc != 0) return rc;
    std::unique_lock lock(mutex_);
    const bool got = changed_.wait_for(lock, options_.commandTimeout, [&] { return ack_ && ack_->first == command; });
    if (!got || !ack_) return -ETIMEDOUT;
    const auto code = ack_->second;
    if (result != nullptr) *result = code;
    return code == 0 ? 0 : -EPERM;
}

IOResult Vehicle::arm(bool force) {
    return sendCommand(kCmdComponentArmDisarm, {1, force ? 21196.0 : 0.0, 0, 0, 0, 0, 0});
}
IOResult Vehicle::disarm(bool force) {
    return sendCommand(kCmdComponentArmDisarm, {0, force ? 21196.0 : 0.0, 0, 0, 0, 0, 0});
}
IOResult Vehicle::setMode(std::uint32_t customMode) {
    return sendCommand(kCmdDoSetMode, {1, static_cast<double>(customMode), 0, 0, 0, 0, 0});
}
IOResult Vehicle::takeoff(double altitudeMeters) {
    return sendCommand(kCmdNavTakeoff, {0, 0, 0, std::nan(""), 0, 0, altitudeMeters});
}
IOResult Vehicle::land() {
    return sendCommand(kCmdNavLand, {0, 0, 0, 0, 0, 0, 0});
}
IOResult Vehicle::returnToLaunch() {
    return sendCommand(kCmdNavReturnToLaunch, {0, 0, 0, 0, 0, 0, 0});
}

IOResult Vehicle::gotoGlobal(double latitude, double longitude, double relativeAltitude) {
    if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) return -EINVAL;
    json::Value f = json::Value::object();
    f["time_boot_ms"] = 0;
    f["target_system"] = options_.targetSystem;
    f["target_component"] = options_.targetComponent;
    f["coordinate_frame"] = 6;
    f["type_mask"] = 0x0DF8;
    f["lat_int"] = static_cast<std::int64_t>(std::llround(latitude * 1e7));
    f["lon_int"] = static_cast<std::int64_t>(std::llround(longitude * 1e7));
    f["alt"] = relativeAltitude;
    return sendMessage("SET_POSITION_TARGET_GLOBAL_INT", f);
}

IOResult Vehicle::requestMessageRate(std::uint32_t messageId, double hertz) {
    const double interval = hertz <= 0.0 ? -1.0 : 1e6 / hertz;
    return sendCommand(kCmdSetMessageInterval, {static_cast<double>(messageId), interval, 0, 0, 0, 0, 0});
}

IOResult Vehicle::setParameter(const std::string& name, double value, std::uint8_t type) {
    if (name.empty() || name.size() > 16) return -EINVAL;
    {
        std::lock_guard lock(mutex_);
        param_.reset();
    }
    json::Value f = json::Value::object();
    f["target_system"] = options_.targetSystem;
    f["target_component"] = options_.targetComponent;
    f["param_id"] = name;
    f["param_value"] = value;
    f["param_type"] = type;
    if (const auto rc = sendMessage("PARAM_SET", f); rc != 0) return rc;
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, options_.commandTimeout, [&] { return param_ && param_->first == name; }) || !param_) return -ETIMEDOUT;
    return std::fabs(param_->second - value) < 1e-3 * std::max(1.0, std::fabs(value)) ? 0 : -EIO;
}

IOResult Vehicle::readParameter(const std::string& name, double& value, std::chrono::milliseconds timeout) {
    if (name.empty() || name.size() > 16) return -EINVAL;
    {
        std::lock_guard lock(mutex_);
        param_.reset();
    }
    json::Value f = json::Value::object();
    f["target_system"] = options_.targetSystem;
    f["target_component"] = options_.targetComponent;
    f["param_id"] = name;
    f["param_index"] = -1;
    if (const auto rc = sendMessage("PARAM_REQUEST_READ", f); rc != 0) return rc;
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, timeout, [&] { return param_ && param_->first == name; }) || !param_) return -ETIMEDOUT;
    value = param_->second;
    return 0;
}

void Vehicle::onMessage(std::function<void(const std::string&, const Frame&, const json::Value&)> handler) {
    std::lock_guard lock(mutex_);
    handler_ = std::move(handler);
}

std::optional<json::Value> Vehicle::lastMessage(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = last_.find(name);
    if (it == last_.end()) return std::nullopt;
    return it->second;
}

ParserStats Vehicle::parserStats() const {
    std::lock_guard lock(mutex_);
    return parser_.stats();
}

std::optional<std::uint32_t> Vehicle::arducopterMode(std::string_view name) {
    static const std::map<std::string, std::uint32_t, std::less<>> modes = {
        {"STABILIZE", 0}, {"ACRO", 1}, {"ALT_HOLD", 2}, {"AUTO", 3},   {"GUIDED", 4},   {"LOITER", 5}, {"RTL", 6},
        {"CIRCLE", 7},    {"LAND", 9}, {"DRIFT", 11},   {"SPORT", 13}, {"POSHOLD", 16}, {"BRAKE", 17}, {"SMART_RTL", 21},
    };
    auto it = modes.find(name);
    if (it == modes.end()) return std::nullopt;
    return it->second;
}

} // namespace gygax::robotics::mavlink
