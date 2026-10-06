#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <gygax/core/json.hpp>
#include <gygax/net/link.hpp>
#include <gygax/robotics/mavlink.hpp>

namespace gygax::testing_support {

using namespace gygax::robotics::mavlink;
using namespace std::chrono_literals;

inline constexpr auto kFakeAutopilotTelemetryInterval = 100ms;

class FakeAutopilot {
public:
    explicit FakeAutopilot(std::shared_ptr<net::ByteLink> link) : link_(std::move(link)) {
        thread_ = std::jthread([this](const std::stop_token& st) { run(st); });
    }

    std::atomic<bool> armed{false};
    std::atomic<std::uint32_t> mode{0};
    std::atomic<int> commands{0};
    std::atomic<bool> rejectArm{false};
    std::atomic<bool> silentCommands{false};
    std::mutex mutex;
    json::Value lastTarget;
    std::map<std::string, double> params{{"BATT_CAPACITY", 3300.0}, {"WPNAV_SPEED", 500.0}};

private:
    void send(const char* name, const json::Value& fields, std::uint8_t seq) {
        const auto* def = findMessage(name);
        Frame f;
        f.sequence = seq;
        f.systemId = 1;
        f.componentId = 1;
        f.messageId = def->id;
        f.payload = *encodePayload(*def, fields);
        (void)link_->write(serialize(f));
    }

    void run(const std::stop_token& st) {
        Parser parser;
        std::vector<std::uint8_t> chunk;
        auto lastTelemetry = std::chrono::steady_clock::now() - 1s;
        std::uint8_t seq = 0;
        while (!st.stop_requested()) {
            if (link_->read(chunk, 20ms) == 0) {
                for (const auto& f : parser.feed(chunk)) handle(f, seq);
            }
            if (std::chrono::steady_clock::now() - lastTelemetry > kFakeAutopilotTelemetryInterval) {
                lastTelemetry = std::chrono::steady_clock::now();
                json::Value hb = json::Value::object();
                hb["type"] = 2;
                hb["autopilot"] = 3;
                hb["base_mode"] = armed.load() ? 209 : 81;
                hb["custom_mode"] = mode.load();
                hb["system_status"] = 4;
                hb["mavlink_version"] = 3;
                send("HEARTBEAT", hb, seq++);
                json::Value pos = json::Value::object();
                pos["lat"] = 473977420;
                pos["lon"] = 85255040;
                pos["alt"] = 550000;
                pos["relative_alt"] = 12000;
                pos["vx"] = 100;
                pos["vy"] = -200;
                pos["vz"] = 30;
                pos["hdg"] = 27000;
                send("GLOBAL_POSITION_INT", pos, seq++);
                json::Value att = json::Value::object();
                att["roll"] = 0.1;
                att["pitch"] = -0.2;
                att["yaw"] = 3.0;
                send("ATTITUDE", att, seq++);
                json::Value sys = json::Value::object();
                sys["voltage_battery"] = 12100;
                sys["current_battery"] = 1500;
                sys["battery_remaining"] = 87;
                send("SYS_STATUS", sys, seq++);
                json::Value hud = json::Value::object();
                hud["groundspeed"] = 5.5;
                hud["airspeed"] = 6.0;
                hud["climb"] = 0.3;
                send("VFR_HUD", hud, seq++);
                json::Value gps = json::Value::object();
                gps["fix_type"] = 3;
                gps["satellites_visible"] = 14;
                send("GPS_RAW_INT", gps, seq++);
            }
        }
    }

    void ack(int command, int result, std::uint8_t& seq) {
        json::Value a = json::Value::object();
        a["command"] = command;
        a["result"] = result;
        send("COMMAND_ACK", a, seq++);
    }

    void handle(const Frame& f, std::uint8_t& seq) {
        const auto* def = findMessage(f.messageId);
        const auto fields = decodePayload(*def, f.payload);
        const std::string name = def->name;
        if (name == "COMMAND_LONG") {
            ++commands;
            const int command = static_cast<int>(fields.getInt("command"));
            if (silentCommands.load()) return;
            if (command == 400) {
                if (rejectArm.load() && fields.getDouble("param1") == 1.0) {
                    ack(command, 4, seq);
                    return;
                }
                armed = fields.getDouble("param1") == 1.0;
            } else if (command == 176) {
                mode = static_cast<std::uint32_t>(fields.getDouble("param2"));
            }
            ack(command, 0, seq);
        } else if (name == "SET_POSITION_TARGET_GLOBAL_INT") {
            std::lock_guard lock(mutex);
            lastTarget = fields;
        } else if (name == "PARAM_REQUEST_READ") {
            const auto id = fields.getString("param_id");
            std::lock_guard lock(mutex);
            if (params.contains(id)) sendParam(id, params[id], seq);
        } else if (name == "PARAM_SET") {
            const auto id = fields.getString("param_id");
            std::lock_guard lock(mutex);
            params[id] = fields.getDouble("param_value");
            sendParam(id, params[id], seq);
        }
    }

    void sendParam(const std::string& id, double value, std::uint8_t& seq) {
        json::Value p = json::Value::object();
        p["param_id"] = id;
        p["param_value"] = value;
        p["param_type"] = 9;
        p["param_count"] = 2;
        p["param_index"] = 0;
        send("PARAM_VALUE", p, seq++);
    }

    std::shared_ptr<net::ByteLink> link_;
    std::jthread thread_;
};

}
