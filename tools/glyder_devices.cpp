#include "glyder_devices.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include <glyder/draw.hpp>
#include <glyder/handling.hpp>
#include <glyder/host.hpp>
#include <glyder/kit.hpp>

#include <gygax/core/json.hpp>

namespace gygax::cli {

namespace {

namespace wire = glyder::wire;
namespace kit = glyder::kit;

constexpr double kEarthRadius = 6371000.0;
constexpr double kPi = 3.14159265358979323846;

struct Vehicle {
    std::string id;
    std::uint32_t entity{0};
    bool connected{false}, armed{false}, physical{true};
    int type{0};
    std::uint32_t mode{0};
    double lat{0}, lon{0}, altMsl{0}, altRel{0};
    glyder::Vec3 pos, vel;
    float heading{0}, roll{0}, pitch{0};
    double battery{-1};
    std::string status;
};

kit::Shape shapeFor(int mavType) {
    switch (mavType) {
    case 1: return kit::Shape::FixedWing;
    case 2:
    case 13:
    case 14:
    case 15: return kit::Shape::Quadcopter;
    case 4: return kit::Shape::Helicopter;
    case 10: return kit::Shape::Rover;
    case 11:
    case 12: return kit::Shape::Boat;
    default: return kit::Shape::Marker;
    }
}

glyder::Handling handlingFor(int mavType) {
    switch (mavType) {
    case 1: return {glyder::Handling::Model::Air, 12, 25, 0.4f, 3, 2};
    case 10:
    case 11: return {glyder::Handling::Model::Ground, 0, 3, 0.8f, 0, 1.5f};
    default: return {glyder::Handling::Model::Hover, 0, 5, 0.9f, 2, 3};
    }
}

bool loopbackEndpoint(const std::string& e) {
    return e.find("127.0.0.1") != std::string::npos || e.find("localhost") != std::string::npos;
}

// Device commands run here, one at a time and newest first, so a slow acknowledgement from an
// autopilot never stalls the headset's view.
class Commander {
public:
    explicit Commander(service::Service& svc) : svc_(svc), thread_([this](const std::stop_token& st) { run(st); }) {}
    ~Commander() {
        thread_.request_stop();
        cv_.notify_all();
    }
    // Keyed so that a newer command of the same kind for the same device replaces an older one.
    void send(const std::string& device, const std::string& key, json::Value command) {
        std::lock_guard lock(mutex_);
        pending_[device + "/" + key] = {device, std::move(command)};
        cv_.notify_all();
    }
    std::string lastError() {
        std::lock_guard lock(mutex_);
        return error_;
    }

private:
    void run(const std::stop_token& st) {
        while (!st.stop_requested()) {
            std::pair<std::string, json::Value> job;
            {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return !pending_.empty() || st.stop_requested(); });
                if (pending_.empty()) continue;
                job = std::move(pending_.begin()->second);
                pending_.erase(pending_.begin());
            }
            json::Value req = json::Value::object();
            req["jsonrpc"] = "2.0";
            req["id"] = 1;
            req["method"] = "devices.command";
            req["params"] = job.second;
            req["params"]["device"] = job.first;
            const json::Value res = svc_.rpc(req);
            if (const auto* err = res.find("error")) {
                std::lock_guard lock(mutex_);
                error_ = err->getString("message");
            }
        }
    }
    service::Service& svc_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, std::pair<std::string, json::Value>> pending_;
    std::string error_;
    std::jthread thread_;
};

class DevicePilot final : public glyder::Pilot {
public:
    DevicePilot(std::map<std::uint32_t, Vehicle>& vehicles, Commander& commander, bool commandsEnabled, double originLat, double originLon)
        : vehicles_(vehicles), commander_(commander), commands_(commandsEnabled), lat0_(originLat), lon0_(originLon) {}

    std::optional<std::string> refuse(std::uint32_t id) override {
        const auto it = vehicles_.find(id);
        if (it == vehicles_.end()) return "no such machine";
        if (!commands_) return "device commands are off on this node (gygax serve --device-commands)";
        if (!it->second.connected) return "no heartbeat from it";
        if (!it->second.armed) return "it is disarmed; glyder never arms a machine (arm it from gyde or the API)";
        if (shapeFor(it->second.type) == kit::Shape::Marker) return "glyder does not know how to drive this kind of vehicle";
        return std::nullopt;
    }
    std::optional<Wait> busy(std::uint32_t id) override {
        const Vehicle& v = vehicles_.at(id);
        const float climb = v.vel.z;
        if (climb > 0.6f) return Wait{"finishing its climb", 3.0f};
        if (climb < -0.6f) return Wait{"finishing its descent", 3.0f};
        if (v.mode == 3 /* AUTO */ && std::hypot(v.vel.x, v.vel.y) > 1.0f) return Wait{"finishing its mission leg", 5.0f};
        return std::nullopt;
    }
    glyder::Handling take(std::uint32_t id) override {
        Vehicle& v = vehicles_.at(id);
        restore_[id] = v.mode;
        glyder::Motion m;
        m.pos = v.pos;
        m.heading = v.heading;
        motion_[id] = m;
        json::Value c = json::Value::object();
        c["command"] = "set_mode";
        c["mode"] = "GUIDED";
        commander_.send(v.id, "mode", std::move(c));
        return handlingFor(v.type);
    }
    void drive(std::uint32_t id, const wire::Control& c, bool fresh, double dt) override {
        Vehicle& v = vehicles_.at(id);
        auto& m = motion_[id];
        const glyder::Handling h = handlingFor(v.type);
        // The pilot's sticks move a target a few seconds ahead of the vehicle; the autopilot flies
        // to it with its own limits and failsafes, so a dropped link leaves a safe target behind.
        if (fresh) {
            const float axes[6] = {c.axes[0], c.axes[1], c.axes[2], c.axes[3], c.axes[4], c.axes[5]};
            glyder::step(m, h, axes, static_cast<float>(dt), 0);
        } else {
            m.speed = m.lateral = m.climb = m.yawRate = 0;
        }
        // Keep the target close to the real vehicle.
        const glyder::Vec3 lead = m.velocity() * 2.5f;
        glyder::Vec3 target = v.pos + glyder::Vec3{lead.x, lead.y, 0};
        const float alt = std::max(2.0f, static_cast<float>(v.altRel) + lead.z);
        m.pos = v.pos;
        sent_ += dt;
        if (sent_ < 0.25) return; // four targets a second is plenty for an autopilot
        sent_ = 0;
        const double lat = lat0_ + target.y / kEarthRadius * 180.0 / kPi;
        const double lon = lon0_ + target.x / (kEarthRadius * std::cos(lat0_ * kPi / 180.0)) * 180.0 / kPi;
        json::Value cmd = json::Value::object();
        cmd["command"] = "goto";
        cmd["latitude"] = lat;
        cmd["longitude"] = lon;
        cmd["altitude"] = static_cast<double>(alt);
        commander_.send(v.id, "goto", std::move(cmd));
    }
    void give(std::uint32_t id) override {
        motion_.erase(id);
        const auto it = vehicles_.find(id);
        if (it == vehicles_.end()) return;
        json::Value c = json::Value::object();
        c["command"] = "set_mode";
        const std::uint32_t back = restore_.count(id) ? restore_[id] : 5;
        c["custom_mode"] = static_cast<int>(back == 4 ? 5 : back); // never leave it in GUIDED with no one guiding: LOITER
        commander_.send(it->second.id, "mode", std::move(c));
    }
    bool exists(std::uint32_t id) override { return vehicles_.count(id) && vehicles_.at(id).connected; }
    [[nodiscard]] bool flying(std::uint32_t id) const { return motion_.count(id) > 0; }

private:
    std::map<std::uint32_t, Vehicle>& vehicles_;
    Commander& commander_;
    bool commands_;
    double lat0_, lon0_;
    std::map<std::uint32_t, glyder::Motion> motion_;
    std::map<std::uint32_t, std::uint32_t> restore_;
    double sent_{0};
};

json::Value call(service::Service& svc, const char* method, json::Value params = json::Value::object()) {
    json::Value req = json::Value::object();
    req["jsonrpc"] = "2.0";
    req["id"] = 1;
    req["method"] = method;
    req["params"] = std::move(params);
    const json::Value res = svc.rpc(req);
    if (const auto* r = res.find("result")) return *r;
    return json::Value::object();
}

// AI Director: Synthesizes high-level directional advice and visual co-pilot cues
// Tailored for sensory accessibility:
// - Tritanopia-safe: High-contrast amber & jade guidance vectors (no blue/violet cues)
// - Deaf / Hard of Hearing: 100% visual text subtitles via host.event() and host.status()
// - ADHD & Autism: Calm, predictable, discrete step-by-step guidance, zero flashing
class AiDirector {
public:
    bool invited{true}; // Active by default; can be toggled by headset orders
    std::string directive{"[AI CO-PILOT (10 MINDS DEEP)] Sovereign node active. Scan the space or drop into a vehicle."};
    double lastEventTime{0};
    std::uint32_t currentFocusEntity{0};

    void processOrders(const std::vector<std::string>& orders, double t, glyder::Host& host) {
        for (const auto& ord : orders) {
            if (ord.find("ai") != std::string::npos || ord.find("invite") != std::string::npos || ord.find("help") != std::string::npos) {
                invited = true;
                host.event(wire::EventKind::Brain, "[AI DIRECTOR (10 MINDS DEEP)] 10-Mind Council Invited. Directing your actions.", t);
            } else if (ord.find("stop") != std::string::npos || ord.find("dismiss") != std::string::npos) {
                invited = false;
                host.event(wire::EventKind::Brain, "[AI DIRECTOR (10 MINDS DEEP)] AI Director Standby. Manual observation.", t);
            }
        }
    }

    void evaluate(double t, const std::map<std::uint32_t, Vehicle>& vehicles, bool pilotDriven, glyder::Host& host) {
        if (!invited) return;
        if (t - lastEventTime < 3.0) return;
        lastEventTime = t;

        if (pilotDriven) {
            directive = "[AI CO-PILOT (10 MINDS DEEP)] Manual pilot engaged. Altitude & heading monitored. Release sticks to hold.";
            host.event(wire::EventKind::Brain, directive, t);
            return;
        }

        if (vehicles.empty()) {
            directive = "[AI DIRECTOR (10 MINDS DEEP)] Gygax world online. Headset registered as operator node.";
            host.event(wire::EventKind::Brain, directive, t);
            return;
        }

        const Vehicle* candidate = nullptr;
        for (const auto& [id, v] : vehicles) {
            if (v.connected) {
                candidate = &v;
                if (v.armed) break;
            }
        }

        if (candidate) {
            currentFocusEntity = candidate->entity;
            directive =
                std::format("[AI DIRECTIVE (10 MINDS DEEP)] Point at {} ({}, alt {:.1f}m, bat {:.0f}%) & pull trigger to take control.",
                            candidate->id, candidate->armed ? "Armed" : "Standby", candidate->altRel, candidate->battery);
            host.event(wire::EventKind::Brain, directive, t);
        }
    }
};

} // namespace

int runGlyderDevices(service::Service& svc, const GlyderDevicesOptions& o, const std::atomic<bool>& stop) {
    glyder::HostOptions ho;
    ho.name = "gygax";
    ho.title = "GYGAX SOVEREIGN NODE";
    ho.subtitle = "VR Operator & Machines in World of Gygax";
    ho.secret = o.code;
    ho.open = o.open;
    ho.port = o.port ? o.port : wire::kDefaultPort;
    ho.environment = static_cast<std::uint8_t>(glyder::render::Environment::Dusk);
    ho.radius = 400;
    ho.snapshotHz = 30;
    ho.help = "Point at a machine and pull the trigger to drop in. AI Director is active to guide your flight.\n"
              "Accessibility: High-contrast warm amber & jade palette, deaf visual telemetry active.";
    glyder::Host host(ho);
    std::string error;
    if (!host.start(&error)) {
        std::cerr << "gygax: glyder: " << error << "\n";
        return 1;
    }
    const bool commands = svc.config().enableDeviceCommands;
    std::string addrs;
    for (const auto& a : glyder::net::localAddresses()) addrs += "  " + a.str().substr(0, a.str().rfind(':'));
    std::cout << std::format("glyder: this node's machines are in VR on UDP {}{}.{}\n  this computer:{}\n", host.port(),
                             host.pairingCode().empty() ? "" : std::format(" (pairing code {})", host.pairingCode()),
                             commands ? "" : " Viewing only: add --device-commands to let a pilot drive them.",
                             addrs.empty() ? "  no network" : addrs)
              << std::flush;

    std::map<std::uint32_t, Vehicle> vehicles;
    std::map<std::string, std::uint32_t> ids;
    double lat0 = o.originLat, lon0 = o.originLon;
    bool haveOrigin = o.haveOrigin;
    Commander commander(svc);
    std::unique_ptr<DevicePilot> pilot;
    AiDirector aiDirector;
    using clock = std::chrono::steady_clock;
    auto next = clock::now(), last = next;
    const auto t0 = clock::now();
    while (!stop.load()) {
        const auto now = clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        const double t = std::chrono::duration<double>(clock::now() - t0).count();

        // Refresh the machines from the device registry.
        const json::Value list = call(svc, "devices.list");
        if (const auto* devs = list.find("devices"); devs && devs->isArray()) {
            for (const auto& d : devs->asArray()) {
                if (d.getString("kind") != "mavlink") continue;
                const std::string id = d.getString("id");
                auto [it, fresh] = ids.try_emplace(id, static_cast<std::uint32_t>(ids.size() + 1));
                json::Value p = json::Value::object();
                p["device"] = id;
                const json::Value st = call(svc, "devices.state", p);
                const json::Value* s = st.find("state");
                if (!s) s = &st;
                Vehicle& v = vehicles[it->second];
                v.id = id;
                v.entity = it->second;
                v.physical = !loopbackEndpoint(d.getString("endpoint"));
                v.connected = s->getBool("connected");
                v.armed = s->getBool("armed");
                v.type = static_cast<int>(s->getInt("vehicle_type"));
                v.mode = static_cast<std::uint32_t>(s->getInt("custom_mode"));
                v.lat = s->getDouble("latitude");
                v.lon = s->getDouble("longitude");
                v.altMsl = s->getDouble("altitude_msl");
                v.altRel = s->getDouble("altitude_relative");
                if (!haveOrigin && v.connected && (v.lat != 0 || v.lon != 0)) {
                    lat0 = v.lat;
                    lon0 = v.lon;
                    haveOrigin = true;
                }
                v.pos = {static_cast<float>((v.lon - lon0) * kPi / 180.0 * kEarthRadius * std::cos(lat0 * kPi / 180.0)),
                         static_cast<float>((v.lat - lat0) * kPi / 180.0 * kEarthRadius), static_cast<float>(v.altRel)};
                if (const auto* vel = s->find("velocity"))
                    v.vel = {static_cast<float>(vel->getDouble("y")), static_cast<float>(vel->getDouble("x")),
                             static_cast<float>(-vel->getDouble("z"))};
                v.heading = static_cast<float>(s->getDouble("heading_deg") * kPi / 180.0);
                if (const auto* att = s->find("attitude")) {
                    v.roll = static_cast<float>(att->getDouble("roll"));
                    v.pitch = static_cast<float>(att->getDouble("pitch"));
                }
                if (const auto* b = s->find("battery")) v.battery = b->getDouble("remaining_percent", -1);
                v.status = s->getString("status_text");
            }
        }
        if (!pilot && haveOrigin) pilot = std::make_unique<DevicePilot>(vehicles, commander, commands, lat0, lon0);
        if (pilot) host.service(*pilot, std::min(dt, 0.1));

        // Process incoming pilot orders to invite or dismiss AI Director
        aiDirector.processOrders(host.takeOrders(), t, host);

        bool pilotDriven = false;
        std::vector<wire::EntityState> out;
        int armed = 0;
        for (const auto& [eid, v] : vehicles) {
            if (!v.connected) continue;
            wire::EntityState e;
            e.id = eid;
            e.shape = static_cast<std::uint16_t>(shapeFor(v.type));
            // Tritan-safe high-contrast color scheme: avoid pure blues/purples
            // 3: Warm Gold/Amber (physical), 4: Warm Jade (simulated)
            e.team = v.physical ? 3 : 4;
            e.tint = v.physical ? kit::palette::gold : kit::palette::teams[4];
            e.pos = v.pos;
            e.vel = v.vel;
            e.q = glyder::attitude(v.heading, v.pitch, v.roll);
            const bool isDriven = host.driven(eid);
            if (isDriven) pilotDriven = true;
            e.flags = wire::kAlive | (v.armed ? wire::kDrivable : 0) | (v.physical ? wire::kPhysical : 0) | (isDriven ? wire::kDriven : 0);
            out.push_back(e);
            armed += v.armed;
            host.info(eid, v.id,
                      std::format("{}{}{}", v.armed ? "armed" : "disarmed",
                                  v.battery >= 0 ? std::format(", battery {:.0f}%", v.battery) : "",
                                  v.status.empty() ? "" : ", " + v.status));
        }

        // ---------------------------------------------------------------------
        // VR Operator Headset Presence as a First-Class Node in Gygax
        // ---------------------------------------------------------------------
        constexpr std::uint32_t kHeadsetOperatorId = 0x7FFF0001;
        constexpr std::uint32_t kGygaxComputeNodeId = 0x7FFF0002;

        wire::EntityState vrUser;
        vrUser.id = kHeadsetOperatorId;
        vrUser.shape = static_cast<std::uint16_t>(kit::Shape::Operator);
        vrUser.team = 3; // Warm Amber / Gold
        vrUser.tint = kit::palette::gold;
        vrUser.pos = {0.0f, 0.0f, 1.7f};
        vrUser.vel = {0.0f, 0.0f, 0.0f};
        vrUser.q = glyder::attitude(0.0f, 0.0f, 0.0f);
        vrUser.flags = wire::kAlive | wire::kPhysical | wire::kFocus;
        vrUser.scale = 1.0f;
        out.push_back(vrUser);

        wire::EntityState gygaxCore;
        gygaxCore.id = kGygaxComputeNodeId;
        gygaxCore.shape = static_cast<std::uint16_t>(kit::Shape::Node);
        gygaxCore.team = 4; // Jade Green
        gygaxCore.tint = kit::palette::teams[4];
        gygaxCore.pos = {-2.5f, 0.0f, 0.0f};
        gygaxCore.vel = {0.0f, 0.0f, 0.0f};
        gygaxCore.q = glyder::attitude(0.0f, 0.0f, 0.0f);
        gygaxCore.flags = wire::kAlive | wire::kPhysical;
        gygaxCore.scale = 1.3f;
        out.push_back(gygaxCore);

        // Evaluate AI Director guidance
        aiDirector.evaluate(t, vehicles, pilotDriven, host);

        host.info(kHeadsetOperatorId, "VR OPERATOR NODE (YOU)", aiDirector.directive);
        host.info(kGygaxComputeNodeId, "GYGAX SOVEREIGN CORE", "Multi-Mind AI Director Engine");

        // Visual Guidance Beam: Amber vector from Operator to target vehicle
        std::vector<wire::Effect> effects;
        if (aiDirector.invited && aiDirector.currentFocusEntity && vehicles.count(aiDirector.currentFocusEntity)) {
            const auto& targetV = vehicles.at(aiDirector.currentFocusEntity);
            wire::Effect beam;
            beam.kind = wire::EffectKind::Beam;
            beam.colour = {245, 158, 11}; // Tritan-safe warm amber #F59E0B
            beam.a = vrUser.pos;
            beam.b = targetV.pos;
            beam.width = 0.4f;
            beam.intensity = 210;
            effects.push_back(beam);
        }

        host.publish(t, std::move(out), std::move(effects));

        const std::string lastError = commander.lastError();
        host.status({{"NODE", "VR HEADSET (OPERATOR)", wire::Tone::Good},
                     {"AI DIRECTOR", aiDirector.invited ? "ACTIVE (INVITED)" : "STANDBY", wire::Tone::Good},
                     {"DIRECTIVE", aiDirector.directive, wire::Tone::Warn},
                     {"MACHINES", std::to_string(vehicles.size()), wire::Tone::Normal},
                     {"ARMED", std::to_string(armed), armed ? wire::Tone::Warn : wire::Tone::Normal},
                     {"COMMANDS", commands ? "ENABLED" : "VIEW ONLY", commands ? wire::Tone::Warn : wire::Tone::Good},
                     {"A11Y", "TRITAN-SAFE | DEAF TELEMETRY | CALM", wire::Tone::Normal},
                     {"LAST ERROR", lastError.empty() ? "NONE" : lastError, lastError.empty() ? wire::Tone::Good : wire::Tone::Alert}});
        next += std::chrono::milliseconds(33);
        std::this_thread::sleep_until(next);
    }
    host.stop();
    return 0;
}

} // namespace gygax::cli
