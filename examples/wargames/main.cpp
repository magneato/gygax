#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gygax/collective/consensus.hpp>
#include <gygax/core/json.hpp>
#include <gygax/core/log.hpp>
#include <gygax/inference/backend.hpp>
#include <gygax/net/http.hpp>
#include <gygax/service/mcp.hpp>

import gygax.tools;
import gygax.orchestration;

#include "battlefield.hpp"

using namespace std::chrono_literals;
using gygax::json::Value;
namespace inference = gygax::inference;
namespace net = gygax::net;

namespace {

using wargames::Battlefield;
using wargames::Kind;
using wargames::Vec;

const char* unitKind(Kind kind) {
    switch (kind) {
    case Kind::Bunker: return "bunker";
    case Kind::Turret: return "turret";
    case Kind::Tank: return "tank";
    case Kind::Interceptor: return "interceptor";
    case Kind::Supply: return "supply-drone";
    case Kind::Swarmer: return "swarmer";
    case Kind::Bruiser: return "bruiser";
    case Kind::Spitter: return "spitter";
    }
    return "unknown";
}

std::uint32_t unitId(const Battlefield& field, const Value& input) {
    const auto* unit = field.findByName(input.getString("unit"));
    if (unit == nullptr) throw std::runtime_error("no such living unit '" + input.getString("unit") + "'");
    return unit->id;
}

Value input(const std::string& text) {
    if (auto parsed = gygax::json::parse(text); parsed && parsed->isObject()) return std::move(*parsed);
    Value v = Value::object();
    v["unit"] = text;
    return v;
}

class ToolBelt {
public:
    explicit ToolBelt(Battlefield& field) : field_(field) {
        auto& registry = gygax::tools::ToolRegistry::getInstance();
        add(registry, "war.observe", "Sensor picture for a unit. Input: {\"unit\": name}", [this](const std::string& in) {
            const auto v = input(in);
            return field_.observe(unitId(field_, v)).dump();
        });
        add(registry, "war.situation", "Command-level picture: inventory, materiel, wave, bunker health, directives",
            [this](const std::string&) { return field_.commanderView().dump(); });
        add(registry, "war.plan", "Range-aware route with refuel stops. Input: {\"unit\", \"x\", \"y\", \"return\": bool}",
            [this](const std::string& in) {
                const auto v = input(in);
                return field_.plan(unitId(field_, v), {v.getDouble("x"), v.getDouble("y")}, v.getBool("return")).dump();
            });
        add(registry, "war.report", "Report the most threatened sector to the collective. Input: {\"unit\", \"sector\", \"confidence\"}",
            [this](const std::string& in) {
                const auto v = input(in);
                const auto id = unitId(field_, v);
                gygax::CollectiveBrain::getInstance().submitHypothesis(
                    {id, v.getString("sector"), static_cast<float>(v.getDouble("confidence"))});
                return std::string("{\"ok\":true}");
            });
        add(registry, "war.act",
            "Issue orders. Input: {\"unit\", \"orders\": [{\"fire\": id} | {\"move\": [x, y]} | {\"resupply\": unit}]}",
            [this](const std::string& in) {
                const auto v = input(in);
                const auto* orders = v.find("orders");
                return field_.applyOrders(unitId(field_, v), orders == nullptr ? Value::array() : *orders);
            });
        add(registry, "war.consensus", "Sector the collective believes is most threatened", [](const std::string&) {
            const auto ranked = gygax::CollectiveBrain::getInstance().report();
            Value out = Value::object();
            out["sector"] = ranked.empty() ? std::string("IDLE") : ranked.front().content;
            out["confidence"] = ranked.empty() ? 0.0 : ranked.front().confidence;
            out["votes"] = ranked.empty() ? std::size_t{0} : ranked.front().votes;
            return out.dump();
        });
        add(registry, "war.rally", "Designate the sector mobile units rally to. Input: {\"sector\"}", [this](const std::string& in) {
            field_.designate(input(in).getString("sector", in));
            return std::string("{\"ok\":true}");
        });
        add(registry, "war.produce", "Order the factory to build a unit. Input: {\"kind\": interceptor|tank|supply-drone}",
            [this](const std::string& in) { return field_.produce(input(in).getString("kind", in)); });
    }

    ~ToolBelt() {
        auto& registry = gygax::tools::ToolRegistry::getInstance();
        for (const auto& name : names_) registry.unregisterTool(name);
    }

    ToolBelt(const ToolBelt&) = delete;
    ToolBelt& operator=(const ToolBelt&) = delete;

private:
    void add(gygax::tools::ToolRegistry& registry, const std::string& name, const std::string& description, gygax::tools::ToolFunction fn) {
        registry.registerTool(name, std::move(fn), description);
        names_.push_back(name);
    }

    Battlefield& field_;
    std::vector<std::string> names_;
};

std::string toolCall(const std::string& tool, Value arguments) {
    Value call = Value::object();
    call["tool"] = tool;
    call["input"] = std::move(arguments);
    return call.dump();
}

std::string answer(const std::string& text) {
    Value v = Value::object();
    v["answer"] = text;
    return v.dump();
}

Value unitArgs(const std::string& unit) {
    Value v = Value::object();
    v["unit"] = unit;
    return v;
}

Value moveOrder(double x, double y) {
    Value order = Value::object();
    Value target = Value::array();
    target.push(std::round(x * 10.0) / 10.0);
    target.push(std::round(y * 10.0) / 10.0);
    order["move"] = std::move(target);
    return order;
}

Value fireOrder(std::int64_t id) {
    Value order = Value::object();
    order["fire"] = id;
    return order;
}

std::string actCall(const std::string& unit, Value orders) {
    Value args = unitArgs(unit);
    args["orders"] = std::move(orders);
    return toolCall("war.act", std::move(args));
}

class DoctrineBackend final : public inference::Backend {
public:
    [[nodiscard]] std::string kind() const override { return "doctrine"; }
    [[nodiscard]] std::string endpoint() const override { return "builtin:doctrine"; }
    [[nodiscard]] bool acceptsAnyModel() const override { return true; }

    inference::ChatResult chat(const inference::ChatRequest& request) override {
        inference::ChatResult result;
        result.ok = true;
        result.status = 200;
        result.model = "doctrine";
        result.engine = "doctrine";
        result.text = decide(request.messages);
        return result;
    }

    bool listModels(std::vector<std::string>& out, std::string&) override {
        out = {"doctrine"};
        return true;
    }

private:
    struct Context {
        std::string unit;
        std::string role;
        std::optional<Value> observation;
        std::optional<Value> plan;
        std::optional<Value> consensus;
        std::optional<Value> situation;
        bool reported = false;
        bool planned = false;
        bool acted = false;
        bool rallied = false;
        bool produced = false;
        bool consensusAsked = false;
    };

    static std::string field(const std::string& text, const std::string& key) {
        const auto at = text.find(key + "=");
        if (at == std::string::npos) return {};
        const auto start = at + key.size() + 1;
        return text.substr(start, text.find_first_of(" .", start) - start);
    }

    static Context parse(const std::vector<inference::Message>& messages) {
        Context c;
        for (const auto& m : messages) {
            if (m.role == "user" && m.content.rfind("Tool result (", 0) != 0 && c.unit.empty()) {
                c.unit = field(m.content, "unit");
                c.role = field(m.content, "role");
            } else if (m.role == "user") {
                const auto close = m.content.find("): ");
                if (close == std::string::npos) continue;
                const auto tool = m.content.substr(13, close - 13);
                auto value = gygax::json::parse(m.content.substr(close + 3));
                if (tool == "war.observe")
                    c.observation = value;
                else if (tool == "war.plan")
                    c.plan = value;
                else if (tool == "war.consensus")
                    c.consensus = value;
                else if (tool == "war.situation")
                    c.situation = value;
            } else if (m.role == "assistant") {
                c.reported = c.reported || m.content.find("war.report") != std::string::npos;
                c.planned = c.planned || m.content.find("war.plan") != std::string::npos;
                c.acted = c.acted || m.content.find("war.act") != std::string::npos;
                c.rallied = c.rallied || m.content.find("war.rally") != std::string::npos;
                c.produced = c.produced || m.content.find("war.produce") != std::string::npos;
                c.consensusAsked = c.consensusAsked || m.content.find("war.consensus") != std::string::npos;
            }
        }
        return c;
    }

    static std::int64_t chooseTarget(const Value& obs, double range, const std::string& priority, std::int64_t focus, bool tight) {
        const auto* contacts = obs.find("contacts");
        if (contacts == nullptr) return 0;
        std::int64_t best = 0;
        double bestKey = -1e18;
        for (const auto& c : contacts->asArray()) {
            const double dist = c.getDouble("dist");
            if (dist > range) continue;
            const auto id = c.getInt("id");
            if (focus != 0 && id == focus) return id;
            const auto kind = c.getString("kind");
            double key;
            if (priority == "nearest") {
                key = -dist;
            } else {
                double weight = static_cast<double>(c.getInt("score"));
                if (priority == kind) weight += 10.0;
                key = weight * 1000.0 - c.getDouble("hp") - dist * 0.01;
            }
            if (tight && c.getInt("score") < 2 && dist > range * 0.5) continue;
            if (key > bestKey) {
                bestKey = key;
                best = id;
            }
        }
        return best;
    }

    static const Value* nearestContact(const Value& obs, const std::string& priority) {
        const auto* contacts = obs.find("contacts");
        if (contacts == nullptr || contacts->asArray().empty()) return nullptr;
        const Value* best = nullptr;
        double bestKey = -1e18;
        for (const auto& c : contacts->asArray()) {
            double weight = static_cast<double>(c.getInt("score"));
            if (priority == c.getString("kind")) weight += 10.0;
            const double key = weight * 1000.0 - c.getDouble("dist");
            if (key > bestKey) {
                bestKey = key;
                best = &c;
            }
        }
        return best;
    }

    static std::string dominantSector(const Value& obs, double& confidence) {
        const auto* sectors = obs.find("sectors");
        std::string best;
        std::int64_t most = 0;
        std::int64_t total = 0;
        if (sectors != nullptr) {
            for (const auto& [name, n] : sectors->asObject()) {
                total += n.asInt();
                if (n.asInt() > most) {
                    most = n.asInt();
                    best = name;
                }
            }
        }
        confidence = total == 0 ? 0.0
                                : std::min(0.95, std::round((0.3 + 0.6 * static_cast<double>(most) / static_cast<double>(total) +
                                                             std::min(0.05, static_cast<double>(total) * 0.005)) *
                                                            20.0) /
                                                     20.0);
        return best;
    }

    static Vec siteAt(const Value& obs, const std::string& id) {
        if (const auto* sites = obs.find("sites")) {
            for (const auto& s : sites->asArray()) {
                if (s.getString("id") == id) return {s.getDouble("x"), s.getDouble("y")};
            }
        }
        return {};
    }

    static Vec nearestSite(const Value& obs, bool depotOnly) {
        Vec best;
        double bestD = 1e18;
        if (const auto* sites = obs.find("sites")) {
            for (const auto& s : sites->asArray()) {
                if (depotOnly && s.getString("kind") != "depot") continue;
                if (s.getDouble("dist") < bestD) {
                    bestD = s.getDouble("dist");
                    best = {s.getDouble("x"), s.getDouble("y")};
                }
            }
        }
        return best;
    }

    std::string decide(const std::vector<inference::Message>& messages) {
        const Context c = parse(messages);
        if (c.role == "commander") return commander(c);
        if (!c.observation) return toolCall("war.observe", unitArgs(c.unit));
        const Value& obs = *c.observation;
        const auto* self = obs.find("self");
        const auto* dir = obs.find("directives");
        if (self == nullptr || dir == nullptr) return answer("no picture");
        const bool combat = c.role == "turret" || c.role == "tank" || c.role == "interceptor";
        if (combat && !c.reported && obs.getInt("visible") > 0) {
            double confidence = 0.0;
            const auto sector = dominantSector(obs, confidence);
            if (!sector.empty()) {
                Value args = unitArgs(c.unit);
                args["sector"] = sector;
                args["confidence"] = confidence;
                return toolCall("war.report", std::move(args));
            }
        }
        if (c.role == "turret") return turret(c, obs, *self, *dir);
        if (c.role == "tank") return tank(c, obs, *self, *dir);
        if (c.role == "interceptor") return interceptor(c, obs, *self, *dir);
        if (c.role == "supply") return supply(c, obs, *self);
        return answer("no doctrine for role " + c.role);
    }

    static std::string turret(const Context& c, const Value& obs, const Value& self, const Value& dir) {
        if (c.acted) return answer("engaged");
        const auto target =
            chooseTarget(obs, self.getDouble("range"), dir.getString("priority"), dir.getInt("focus"), dir.getString("weapons") == "tight");
        if (target == 0 || dir.getString("weapons") == "hold") return answer("holding");
        Value orders = Value::array();
        orders.push(fireOrder(target));
        return actCall(c.unit, std::move(orders));
    }

    static std::string tank(const Context& c, const Value& obs, const Value& self, const Value& dir) {
        if (c.acted) return answer("maneuvering");
        Value orders = Value::array();
        const bool weak = self.getDouble("hp_frac") < 0.3 || self.getDouble("ammo_frac") < 0.2;
        const Vec here{self.getDouble("x"), self.getDouble("y")};
        if (weak) {
            orders.push(moveOrder(0.0, -40.0));
        } else {
            const bool holdFire = dir.getString("weapons") == "hold";
            const auto target = holdFire ? 0
                                         : chooseTarget(obs, self.getDouble("range"), dir.getString("priority"), dir.getInt("focus"),
                                                        dir.getString("weapons") == "tight");
            if (target != 0) {
                orders.push(fireOrder(target));
            } else if (const auto* rally = obs.find("rally")) {
                orders.push(moveOrder(rally->getDouble("x"), rally->getDouble("y")));
            } else if (const Value* contact = nearestContact(obs, dir.getString("priority")); contact != nullptr && !holdFire) {
                Vec goal{contact->getDouble("x"), contact->getDouble("y")};
                const double norm = std::hypot(goal.x, goal.y);
                if (norm > 200.0) goal = {goal.x / norm * 200.0, goal.y / norm * 200.0};
                orders.push(moveOrder(goal.x, goal.y));
            } else if (std::hypot(here.x, here.y + 40.0) > 30.0) {
                orders.push(moveOrder(0.0, -40.0));
            }
        }
        if (orders.asArray().empty()) return answer("holding");
        return actCall(c.unit, std::move(orders));
    }

    static std::string interceptor(const Context& c, const Value& obs, const Value& self, const Value& dir) {
        const Vec here{self.getDouble("x"), self.getDouble("y")};
        const bool needsAmmo = self.getDouble("ammo_frac") < 0.15;
        const bool needsService = self.getDouble("fuel") < 0.35 || needsAmmo || self.getDouble("hp_frac") < 0.25;
        if (c.acted) return answer("flying");
        if (dir.getString("drones") == "recall" || needsService) {
            const Vec dock = needsAmmo || dir.getString("drones") == "recall" ? siteAt(obs, "hq") : nearestSite(obs, false);
            Value orders = Value::array();
            orders.push(moveOrder(dock.x, dock.y));
            return actCall(c.unit, std::move(orders));
        }
        const bool holdFire = dir.getString("weapons") == "hold";
        const Value* contact =
            holdFire ? nullptr : nearestContact(obs, dir.getString("priority") == "threat" ? "spitter" : dir.getString("priority"));
        if (contact == nullptr) {
            Value orders = Value::array();
            if (const auto* rally = obs.find("rally"))
                orders.push(moveOrder(rally->getDouble("x"), rally->getDouble("y")));
            else
                orders.push(moveOrder(0.0, 30.0));
            return actCall(c.unit, std::move(orders));
        }
        const Vec target{contact->getDouble("x"), contact->getDouble("y")};
        if (!c.planned) {
            Value args = unitArgs(c.unit);
            args["x"] = target.x;
            args["y"] = target.y;
            args["return"] = true;
            return toolCall("war.plan", std::move(args));
        }
        Value orders = Value::array();
        const auto* route = c.plan ? c.plan->find("route") : nullptr;
        if (c.plan && c.plan->getBool("feasible") && route != nullptr && !route->asArray().empty()) {
            const auto& first = route->asArray().front();
            if (first.getString("type") == "refuel") {
                orders.push(moveOrder(first.getDouble("x"), first.getDouble("y")));
            } else {
                const double dist = std::hypot(target.x - here.x, target.y - here.y);
                const double keep = 0.7 * self.getDouble("range");
                const double step = std::max(0.0, dist - keep);
                orders.push(moveOrder(here.x + (target.x - here.x) / std::max(dist, 1.0) * step,
                                      here.y + (target.y - here.y) / std::max(dist, 1.0) * step));
                orders.push(fireOrder(contact->getInt("id")));
            }
        } else {
            const Vec dock = nearestSite(obs, false);
            orders.push(moveOrder(dock.x, dock.y));
        }
        return actCall(c.unit, std::move(orders));
    }

    static std::string supply(const Context& c, const Value& obs, const Value& self) {
        if (c.acted) return answer("delivering");
        const auto* needs = obs.find("needs_ammo");
        const bool hasNeed = needs != nullptr && !needs->asArray().empty();
        const Vec hq = siteAt(obs, "hq");
        Value orders = Value::array();
        if (self.getDouble("cargo") < 80.0 || !hasNeed || self.getDouble("fuel") < 0.25) {
            orders.push(moveOrder(hq.x, hq.y));
            return actCall(c.unit, std::move(orders));
        }
        const auto& need = needs->asArray().front();
        if (!c.planned) {
            Value args = unitArgs(c.unit);
            args["x"] = need.getDouble("x");
            args["y"] = need.getDouble("y");
            args["return"] = true;
            return toolCall("war.plan", std::move(args));
        }
        const auto* route = c.plan ? c.plan->find("route") : nullptr;
        if (c.plan && c.plan->getBool("feasible") && route != nullptr && !route->asArray().empty()) {
            const auto& first = route->asArray().front();
            if (first.getString("type") == "refuel") {
                orders.push(moveOrder(first.getDouble("x"), first.getDouble("y")));
            } else {
                Value deliver = Value::object();
                deliver["resupply"] = need.getString("unit");
                orders.push(std::move(deliver));
            }
        } else {
            orders.push(moveOrder(hq.x, hq.y));
        }
        return actCall(c.unit, std::move(orders));
    }

    static std::string commander(const Context& c) {
        if (!c.situation) return toolCall("war.situation", Value::object());
        if (!c.consensusAsked) return toolCall("war.consensus", Value::object());
        const Value& s = *c.situation;
        if (!c.rallied && c.consensus) {
            const auto sector = c.consensus->getString("sector", "IDLE");
            if (sector != "IDLE" && c.consensus->getDouble("confidence") >= 0.4) {
                Value args = Value::object();
                args["sector"] = sector;
                return toolCall("war.rally", std::move(args));
            }
        }
        if (!c.produced) {
            const auto* inv = s.find("inventory");
            const auto* costs = s.find("costs");
            const double materiel = s.getDouble("materiel");
            std::string want;
            if (const auto* queue = s.find("produce_queue"); queue != nullptr && !queue->asArray().empty()) {
                want = queue->asArray().front().asString();
            } else if (s.getBool("autoproduce") && inv != nullptr) {
                if (inv->getInt("supply-drone") < 2)
                    want = "supply-drone";
                else if (inv->getInt("interceptor") < 5)
                    want = "interceptor";
                else if (inv->getInt("tank") < 3)
                    want = "tank";
                else if (inv->getInt("interceptor") < 8)
                    want = "interceptor";
            }
            if (!want.empty() && costs != nullptr && costs->getDouble(want, 1e9) <= materiel) {
                Value args = Value::object();
                args["kind"] = want;
                return toolCall("war.produce", std::move(args));
            }
        }
        return answer(std::format("sector {} watched", c.consensus ? c.consensus->getString("sector", "IDLE") : "IDLE"));
    }
};

class Console {
public:
    Console() : thread_([this](const std::stop_token& stop) { loop(stop); }) {}

    std::optional<std::string> next(std::chrono::milliseconds wait) {
        std::unique_lock lock(mutex_);
        ready_.wait_for(lock, wait, [this] { return !lines_.empty() || closed_; });
        if (lines_.empty()) return std::nullopt;
        auto line = std::move(lines_.front());
        lines_.pop_front();
        return line;
    }

    [[nodiscard]] bool closed() {
        std::lock_guard lock(mutex_);
        return closed_ && lines_.empty();
    }

private:
    void loop(const std::stop_token& stop) {
        std::string pending;
        char buffer[512];
        while (!stop.stop_requested()) {
            pollfd p{STDIN_FILENO, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) continue;
            const auto n = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (n <= 0) break;
            pending.append(buffer, static_cast<std::size_t>(n));
            std::size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::lock_guard lock(mutex_);
                lines_.push_back(pending.substr(0, nl));
                pending.erase(0, nl + 1);
                ready_.notify_all();
            }
        }
        std::lock_guard lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> lines_;
    bool closed_ = false;
    std::jthread thread_;
};

struct ScriptLine {
    double time = 0.0;
    std::string directive;
};

std::vector<ScriptLine> loadScript(const std::string& path, std::string& error) {
    std::vector<ScriptLine> out;
    std::ifstream in(path);
    if (!in) {
        error = "cannot read script " + path;
        return out;
    }
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream words(line);
        double t = 0.0;
        if (!(words >> t)) {
            error = "script line must start with a time in seconds: " + line;
            return out;
        }
        std::string rest;
        std::getline(words, rest);
        const auto start = rest.find_first_not_of(' ');
        out.push_back({t, start == std::string::npos ? std::string() : rest.substr(start)});
    }
    std::ranges::stable_sort(out, {}, &ScriptLine::time);
    return out;
}

struct RunConfig {
    wargames::Options game;
    bool map = false;
    bool quiet = false;
    bool interactive = false;
    double pace = 0.0;
    std::vector<ScriptLine> script;
    std::string jsonPath;
    std::string htmlPath;
    std::string engine;
    std::string model = "doctrine";
};

struct RunResult {
    wargames::Outcome outcome;
    wargames::Stats stats;
    std::uint64_t digest = 0;
    std::uint64_t modelCalls = 0;
    std::uint64_t toolCalls = 0;
    std::uint64_t failedRuns = 0;
    Value ledger;
    std::map<std::string, int> alive;
    std::map<std::string, int> ledgerActive;
    double bunkerHp = 0.0;
    std::vector<std::pair<std::string, Vec>> positions;
    std::vector<std::string> log;
};

const char* helpText() {
    return "directives (issued to the commander and every unit agent):\n"
           "  rally <N|NE|E|SE|S|SW|W|NW|auto>   mobile units rally to a sector (auto = agent consensus)\n"
           "  priority <threat|spitter|bruiser|swarmer|nearest>\n"
           "  weapons <free|tight|hold>           tight conserves ammo, hold ceases fire\n"
           "  drones <hunt|recall>                interceptors hunt or return to HQ\n"
           "  focus <alien id|off>                every weapon focuses one target\n"
           "  produce <interceptor|tank|supply-drone>\n"
           "  autoproduce <on|off>\n"
           "  note <text>                         free text placed in every agent's orders\n"
           "console: status, map, pause, resume, step [n], pace <seconds per tick>, help, quit\n";
}

class Game {
public:
    explicit Game(RunConfig config) : config_(std::move(config)), field_(config_.game), tools_(field_) {
        gygax::inference::OrchestratorOptions options;
        options.workers = 1;
        options.defaultMaxSteps = 8;
        options.defaultModel = config_.model;
        std::shared_ptr<inference::Backend> backend = std::make_shared<DoctrineBackend>();
        if (!config_.engine.empty()) {
            std::string error;
            backend = inference::makeBackend(config_.engine, &error);
            if (!backend) throw std::runtime_error(error);
        }
        orchestrator_ = std::make_unique<inference::Orchestrator>(std::move(backend), options);
        orchestrator_->start();
    }

    ~Game() { orchestrator_->stop(); }

    RunResult run() {
        captureFrame();
        std::unique_ptr<Console> console;
        if (config_.interactive) {
            console = std::make_unique<Console>();
            std::cout << helpText() << "\n";
        }
        std::size_t scriptAt = 0;
        bool paused = config_.interactive;
        int steps = 0;
        bool quit = false;
        while (!field_.outcome().finished && !quit) {
            if (console) {
                while (auto line = console->next(paused && steps == 0 ? 100ms : 0ms)) {
                    handleConsole(*line, paused, steps, quit);
                }
                if (quit) break;
                if (paused && steps == 0) {
                    if (console->closed()) paused = false;
                    continue;
                }
                if (steps > 0) --steps;
            }
            while (scriptAt < config_.script.size() && config_.script[scriptAt].time <= field_.time()) {
                emit(std::format("[script {:.0f}s] {} -> {}", config_.script[scriptAt].time, config_.script[scriptAt].directive,
                                 field_.directive(config_.script[scriptAt].directive)));
                ++scriptAt;
            }
            tick();
            captureFrame();
            if (config_.interactive && field_.tick() % 5 == 0) emit(field_.status());
            if (config_.pace > 0.0) std::this_thread::sleep_for(std::chrono::duration<double>(config_.pace));
        }
        return finish();
    }

    bool writeHtml(std::string& error) const {
        if (config_.htmlPath.empty()) return true;
        std::ifstream viewer(GYGAX_WARGAMES_VIEWER);
        if (!viewer) {
            error = std::format("cannot read replay viewer template: {}", GYGAX_WARGAMES_VIEWER);
            return false;
        }
        std::string html((std::istreambuf_iterator<char>(viewer)), std::istreambuf_iterator<char>());
        constexpr std::string_view marker = "/* GYGAX_REPLAY_DATA */";
        const auto at = html.find(marker);
        if (at == std::string::npos) {
            error = "replay viewer template is missing its data marker";
            return false;
        }
        std::string replay = frames_.dump();
        for (const char character : {'<', '>', '&'}) {
            const std::string escaped = character == '<' ? "\\u003c" : character == '>' ? "\\u003e" : "\\u0026";
            std::size_t pos = 0;
            while ((pos = replay.find(character, pos)) != std::string::npos) {
                replay.replace(pos, 1, escaped);
                pos += escaped.size();
            }
        }
        html.replace(at, marker.size(), replay);
        std::ofstream out(config_.htmlPath);
        if (!out) {
            error = std::format("cannot write replay HTML: {}", config_.htmlPath);
            return false;
        }
        out << html;
        if (!out) {
            error = std::format("failed while writing replay HTML: {}", config_.htmlPath);
            return false;
        }
        return true;
    }

private:
    void captureFrame() {
        Value frame = Value::object();
        frame["time"] = field_.time();
        frame["tick"] = field_.tick();
        frame["wave"] = field_.outcome().wavesSpawned;
        frame["wave_limit"] = field_.options().waves;
        frame["finished"] = field_.outcome().finished;
        frame["victory"] = field_.outcome().victory;
        frame["bunker_hp"] = field_.bunkerHp();
        frame["materiel"] = field_.materiel();
        frame["hive_rage"] = field_.hiveRage();

        const auto& stats = field_.stats();
        Value statsValue = Value::object();
        statsValue["aliens_spawned"] = stats.aliensSpawned;
        statsValue["aliens_killed"] = stats.aliensKilled;
        statsValue["humans_lost"] = stats.humansLost;
        statsValue["humans_built"] = stats.humansBuilt;
        statsValue["shots_fired"] = stats.shotsFired;
        statsValue["ammo_delivered"] = stats.ammoDelivered;
        statsValue["damage_dealt"] = stats.damageDealt;
        statsValue["damage_taken"] = stats.damageTaken;
        const auto agentStats = orchestrator_->stats();
        statsValue["model_calls"] = agentStats.modelCalls;
        statsValue["tool_calls"] = agentStats.toolCalls;
        statsValue["failed_runs"] = failedRuns_ + agentStats.failed;
        frame["stats"] = std::move(statsValue);

        const auto directives = field_.directives();
        Value directiveValue = Value::object();
        directiveValue["weapons"] = directives.weapons;
        directiveValue["priority"] = directives.priority;
        directiveValue["drones"] = directives.drones;
        directiveValue["rally"] = directives.rally;
        directiveValue["rally_human"] = directives.rallyHuman;
        directiveValue["autoproduce"] = directives.autoProduce;
        directiveValue["focus"] = directives.focus;
        Value notes = Value::array();
        for (const auto& note : directives.notes) notes.push(note);
        directiveValue["notes"] = std::move(notes);
        frame["directives"] = std::move(directiveValue);

        Value consensus = Value::array();
        for (const auto& entry : gygax::CollectiveBrain::getInstance().report()) {
            Value item = Value::object();
            item["sector"] = entry.content;
            item["confidence"] = entry.confidence;
            item["votes"] = entry.votes;
            consensus.push(std::move(item));
        }
        frame["consensus"] = std::move(consensus);

        Value units = Value::array();
        std::map<std::string, std::pair<int, int>> unitTotals;
        for (const auto& entity : field_.entities()) {
            auto& totals = unitTotals[unitKind(entity.kind)];
            entity.alive ? ++totals.first : ++totals.second;
            if (!entity.alive) continue;
            Value unit = Value::object();
            unit["id"] = entity.id;
            unit["name"] = entity.name;
            unit["kind"] = unitKind(entity.kind);
            unit["sku"] = wargames::specOf(entity.kind).sku;
            unit["role"] = wargames::roleOf(entity.kind);
            unit["human"] = wargames::specOf(entity.kind).human;
            unit["alive"] = entity.alive;
            unit["x"] = entity.pos.x;
            unit["y"] = entity.pos.y;
            unit["hp"] = entity.hp;
            unit["hp_max"] = wargames::specOf(entity.kind).hp;
            unit["ammo"] = entity.ammo;
            unit["fuel"] = entity.fuel;
            unit["cargo"] = entity.cargo;
            unit["engaged"] = entity.engaged;
            unit["fire_target"] = entity.orders.fire;
            unit["resupply_target"] = entity.orders.resupply;
            unit["has_destination"] = entity.orders.hasDestination;
            unit["destination_x"] = entity.orders.destination.x;
            unit["destination_y"] = entity.orders.destination.y;
            unit["wave"] = entity.wave;
            units.push(std::move(unit));
        }
        frame["units"] = std::move(units);
        Value totals = Value::object();
        for (const auto& [kind, count] : unitTotals) {
            Value item = Value::object();
            item["active"] = count.first;
            item["lost"] = count.second;
            totals[kind] = std::move(item);
        }
        frame["unit_totals"] = std::move(totals);

        Value events = Value::array();
        for (std::size_t i = frameEventAt_; i < log_.size(); ++i) events.push(log_[i]);
        frameEventAt_ = log_.size();
        frame["events"] = std::move(events);
        frames_.push(std::move(frame));
    }

    void emit(const std::string& line) {
        log_.push_back(line);
        if (!config_.quiet) std::cout << line << "\n";
    }

    void handleConsole(const std::string& line, bool& paused, int& steps, bool& quit) {
        std::istringstream words(line);
        std::string cmd;
        words >> cmd;
        if (cmd.empty()) return;
        if (cmd == "quit" || cmd == "exit") {
            quit = true;
        } else if (cmd == "pause") {
            paused = true;
            steps = 0;
            emit("paused");
        } else if (cmd == "resume") {
            paused = false;
            emit("running");
        } else if (cmd == "step") {
            int n = 1;
            words >> n;
            steps = std::clamp(n, 1, 600);
            paused = true;
        } else if (cmd == "pace") {
            double p = 0.0;
            if (words >> p) config_.pace = std::clamp(p, 0.0, 5.0);
            emit(std::format("pace {:.2f}s per tick", config_.pace));
        } else if (cmd == "status") {
            emit(field_.status());
        } else if (cmd == "map") {
            std::cout << field_.render();
        } else if (cmd == "help") {
            std::cout << helpText();
        } else {
            emit(field_.directive(line));
        }
    }

    std::string objective(const wargames::Entity& e) {
        const auto d = field_.directives();
        std::string text = std::format("unit={} role={} tick={} time={:.0f}. Defend the last bunker against the alien invasion.", e.name,
                                       wargames::roleOf(e.kind), field_.tick(), field_.time());
        if (d.weapons != "free") text += " Weapons " + d.weapons + ".";
        if (d.priority != "threat") text += " Priority " + d.priority + ".";
        for (const auto& n : d.notes) text += " Command note: " + n;
        return text;
    }

    void syncAgents() {
        for (const auto id : field_.newHumanIds()) {
            const auto* e = field_.find(id);
            if (e == nullptr) continue;
            agents_[e->name] = orchestrator_->createAgent(e->name, 8);
        }
        for (const auto& name : field_.takeRetired()) {
            if (auto it = agents_.find(name); it != agents_.end()) {
                orchestrator_->removeAgent(it->second);
                agents_.erase(it);
            }
        }
    }

    void runAgent(const wargames::Entity& e) {
        const auto sid = agents_.at(e.name);
        if (orchestrator_->submit(sid, objective(e), config_.model) != inference::SubmitResult::Accepted) {
            ++failedRuns_;
            return;
        }
        const auto snap = orchestrator_->wait(sid, 30s);
        if (!snap || snap->status != "completed") {
            ++failedRuns_;
            emit(std::format("agent {} failed: {}", e.name, snap ? snap->error : "vanished"));
        }
    }

    void tick() {
        field_.beginTick();
        syncAgents();
        gygax::CollectiveBrain::getInstance().resetPool();
        const auto ids = field_.livingHumanIds();
        const wargames::Entity* commander = nullptr;
        for (const auto id : ids) {
            const auto* e = field_.find(id);
            if (e->kind == Kind::Bunker) {
                commander = e;
                continue;
            }
            runAgent(*e);
        }
        if (commander != nullptr) runAgent(*commander);
        const bool waveArrived = field_.outcome().wavesSpawned != lastWave_;
        field_.endTick();
        for (const auto& line : field_.takeNarrative()) emit(line);
        if (config_.map && (waveArrived || field_.outcome().finished)) {
            lastWave_ = field_.outcome().wavesSpawned;
            if (!config_.quiet) std::cout << field_.render();
        }
        lastWave_ = field_.outcome().wavesSpawned;
    }

    RunResult finish() {
        RunResult r;
        r.outcome = field_.outcome();
        r.stats = field_.stats();
        r.digest = field_.digest();
        const auto stats = orchestrator_->stats();
        r.modelCalls = stats.modelCalls;
        r.toolCalls = stats.toolCalls;
        r.failedRuns = failedRuns_ + stats.failed;
        r.ledger = field_.ledger().summary(0);
        r.bunkerHp = field_.bunkerHp();
        for (const auto& e : field_.entities()) {
            if (!e.alive) continue;
            r.alive[wargames::specOf(e.kind).sku] += 1;
            r.positions.emplace_back(e.name, e.pos);
        }
        if (const auto* skus = r.ledger.find("skus")) {
            for (const auto& s : skus->asArray()) r.ledgerActive[s.getString("sku")] = static_cast<int>(s.getInt("active"));
        }
        r.log = log_;
        return r;
    }

    RunConfig config_;
    Battlefield field_;
    ToolBelt tools_;
    std::unique_ptr<inference::Orchestrator> orchestrator_;
    std::map<std::string, std::uint32_t> agents_;
    std::vector<std::string> log_;
    Value frames_ = Value::array();
    std::uint64_t failedRuns_ = 0;
    int lastWave_ = 0;
    std::size_t frameEventAt_ = 0;
};

Value toJson(const RunResult& r) {
    Value out = Value::object();
    out["victory"] = r.outcome.victory;
    out["finished"] = r.outcome.finished;
    out["time_s"] = r.outcome.time;
    out["waves"] = r.outcome.wavesSpawned;
    out["bunker_hp"] = r.bunkerHp;
    out["aliens_spawned"] = r.stats.aliensSpawned;
    out["aliens_killed"] = r.stats.aliensKilled;
    out["humans_lost"] = r.stats.humansLost;
    out["humans_built"] = r.stats.humansBuilt;
    out["shots_fired"] = r.stats.shotsFired;
    out["ammo_delivered"] = r.stats.ammoDelivered;
    out["model_calls"] = r.modelCalls;
    out["tool_calls"] = r.toolCalls;
    out["digest"] = std::format("{:016x}", r.digest);
    Value alive = Value::object();
    for (const auto& [sku, n] : r.alive) alive[sku] = n;
    out["alive"] = std::move(alive);
    out["ledger"] = r.ledger;
    return out;
}

void printReport(const RunResult& r) {
    std::cout << "\n=== " << (r.outcome.victory ? "HUMANITY HOLDS" : "THE BUNKER HAS FALLEN") << " after "
              << std::format("{:.0f}", r.outcome.time) << " s, " << r.outcome.wavesSpawned << " waves ===\n";
    std::cout << std::format("aliens: {} landed, {} destroyed | defenders: {} lost, {} built | bunker {:.0f}/4000 hp\n",
                             r.stats.aliensSpawned, r.stats.aliensKilled, r.stats.humansLost, r.stats.humansBuilt, r.bunkerHp);
    std::cout << std::format("ammunition: {} rounds fired, {:.0f} delivered by supply drones\n", r.stats.shotsFired, r.stats.ammoDelivered);
    std::cout << std::format("agents: {} model calls, {} tool calls, {} failed runs (deterministic digest {:016x})\n", r.modelCalls,
                             r.toolCalls, r.failedRuns, r.digest);
    std::cout << "supply chain ledger (units in the field):\n";
    if (const auto* skus = r.ledger.find("skus")) {
        for (const auto& s : skus->asArray()) {
            std::cout << std::format("  {:<14} active {:>3}  lost {:>3}  retired {:>3}\n", s.getString("sku"), s.getInt("active"),
                                     s.getInt("lost"), s.getInt("retired"));
        }
    }
}

struct Args {
    RunConfig config;
    bool selfcheck = false;
    bool mcpServe = false;
    bool help = false;
    std::string error;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                a.error = arg + " needs a value";
                return {};
            }
            return argv[++i];
        };
        if (arg == "--seed")
            a.config.game.seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (arg == "--seconds")
            a.config.game.seconds = std::clamp(std::strtod(next().c_str(), nullptr), 10.0, 1800.0);
        else if (arg == "--waves")
            a.config.game.waves = std::clamp(static_cast<int>(std::strtol(next().c_str(), nullptr, 10)), 0, 8);
        else if (arg == "--difficulty")
            a.config.game.difficulty = std::clamp(std::strtod(next().c_str(), nullptr), 0.1, 4.0);
        else if (arg == "--pace")
            a.config.pace = std::clamp(std::strtod(next().c_str(), nullptr), 0.0, 5.0);
        else if (arg == "--script")
            a.config.script = loadScript(next(), a.error);
        else if (arg == "--json")
            a.config.jsonPath = next();
        else if (arg == "--html")
            a.config.htmlPath = next();
        else if (arg == "--map")
            a.config.map = true;
        else if (arg == "--quiet")
            a.config.quiet = true;
        else if (arg == "--interactive")
            a.config.interactive = true;
        else if (arg == "--selfcheck")
            a.selfcheck = true;
        else if (arg == "--engine")
            a.config.engine = next();
        else if (arg == "--model")
            a.config.model = next();
        else if (arg == "--mcp-serve")
            a.mcpServe = true;
        else if (arg == "--help" || arg == "-h")
            a.help = true;
        else
            a.error = "unknown option " + arg;
        if (!a.error.empty()) break;
    }
    if (a.config.interactive && a.config.pace == 0.0) a.config.pace = 0.4;
    return a;
}

void printUsage() {
    std::cout << "wargames - Gygax agents defend the last bunker against an alien invasion\n\n"
                 "usage: wargames [options]\n"
                 "  --seed N          random seed (default 7); same seed and directives give the same battle\n"
                 "  --seconds N       time limit (default 240)\n"
                 "  --waves N         alien waves, 0-8 (default 6)\n"
                 "  --difficulty F    scale alien numbers (default 2.5; 3.0 overruns the bunker)\n"
                 "  --map             print an ASCII map when each wave lands and at the end\n"
                 "  --interactive     start paused and take human directives on stdin (type help)\n"
                 "  --script FILE     human directives with times: '<seconds> <directive>' per line\n"
                 "  --pace S          real seconds per simulated second (default 0; 0.4 when interactive)\n"
                 "  --json FILE       write the final report as JSON\n"
                 "  --html FILE       write a self-contained graphical replay HTML\n"
                 "  --quiet           only print the final report\n"
                 "  --engine SPEC     let a language model decide instead of the built-in doctrine: ollama, llama-cpp, lmstudio\n"
                 "                    (optionally NAME@host:port) or http://host:port/v1; see docs/SERVICE.md\n"
                 "  --model M         model name sent to the engine (default doctrine)\n"
                 "  --mcp-serve       serve the war.* tools as an MCP server on stdin/stdout instead of running a battle\n"
                 "  --selfcheck       run the built-in verification and exit\n\n"
              << helpText();
}

int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << "\n";
    if (!ok) ++failures;
}

class DoctrineServer {
public:
    DoctrineServer() {
        net::ServerOptions o;
        o.port = 0;
        server_ = std::make_unique<net::Server>(o);
        server_->route("GET", "/v1/models", [](net::Request&) { return net::Response::json(200, R"({"data":[{"id":"doctrine"}]})"); });
        server_->route("POST", "/v1/chat/completions", [this](net::Request& r) {
            auto body = gygax::json::parse(r.body);
            if (!body || body->find("messages") == nullptr) return net::Response::error(400, "bad_request", "messages required");
            inference::ChatRequest request;
            for (const auto& m : body->find("messages")->asArray())
                request.messages.push_back({m.getString("role"), m.getString("content")});
            std::lock_guard lock(mutex_);
            const auto result = doctrine_.chat(request);
            Value message = Value::object();
            message["role"] = "assistant";
            message["content"] = result.text;
            Value choice = Value::object();
            choice["message"] = std::move(message);
            Value choices = Value::array();
            choices.push(std::move(choice));
            Value out = Value::object();
            out["model"] = "doctrine";
            out["choices"] = std::move(choices);
            ++requests_;
            return net::Response::json(200, out.dump());
        });
        started_ = server_->start();
    }

    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] std::uint16_t port() const { return static_cast<std::uint16_t>(server_->port()); }
    [[nodiscard]] std::uint64_t requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    std::unique_ptr<net::Server> server_;
    DoctrineBackend doctrine_;
    mutable std::mutex mutex_;
    std::uint64_t requests_ = 0;
    bool started_ = false;
};

std::string selfPath;

int serveMcp() {
    Battlefield field{wargames::Options{}};
    ToolBelt belt(field);
    auto& registry = gygax::tools::ToolRegistry::getInstance();
    gygax::service::McpServerHandler handler;
    handler.list = [&registry] {
        std::vector<gygax::service::McpServerTool> out;
        for (const auto& t : registry.list())
            if (t.name.rfind("war.", 0) == 0) out.push_back({t.name, t.description});
        return out;
    };
    handler.call = [&registry](const std::string& name, const std::string& args) {
        if (name.rfind("war.", 0) != 0) throw std::runtime_error("no such tool");
        const auto out = registry.execute(name, args);
        if (!out) throw std::runtime_error("no such tool");
        return *out;
    };
    return gygax::service::serveMcp(0, 1, "wargames", "1", handler);
}

RunResult quiet(RunConfig config) {
    config.quiet = true;
    Game game(std::move(config));
    return game.run();
}

double distanceToPoint(const RunResult& r, const std::string& prefix, const Vec& p) {
    double best = 1e18;
    for (const auto& [name, pos] : r.positions) {
        if (name.rfind(prefix, 0) == 0) best = std::min(best, std::hypot(pos.x - p.x, pos.y - p.y));
    }
    return best;
}

int selfcheck() {
    std::cout << "wargames self-check\n";
    RunConfig base;
    base.game.seconds = 100.0;
    base.game.waves = 2;
    base.game.difficulty = 0.7;

    const auto first = quiet(base);
    const auto second = quiet(base);
    check(first.digest == second.digest, "same seed and no human input give the identical battle");
    RunConfig other = base;
    other.game.seed = 8;
    check(quiet(other).digest != first.digest, "a different seed gives a different battle");
    check(first.failedRuns == 0 && first.toolCalls > 200, std::format("agents ran cleanly ({} tool calls)", first.toolCalls));
    check(first.stats.aliensKilled > 0 && first.stats.shotsFired > 0, "the defenders engaged the invaders");
    std::map<std::string, int> ledgerLive;
    for (const auto& [sku, n] : first.ledgerActive) {
        if (n > 0) ledgerLive[sku] = n;
    }
    const bool ledgerMatches = ledgerLive == first.alive;
    check(ledgerMatches, "the supply-chain ledger agrees with the battlefield unit for unit");
    check(first.stats.ammoDelivered > 0.0 || first.stats.shotsFired < 3000, "supply drones keep the guns fed");

    RunConfig hold = base;
    hold.script = {{0.0, "weapons hold"}};
    const auto held = quiet(hold);
    check(held.stats.shotsFired == 0 && held.stats.aliensKilled == 0, "a human 'weapons hold' silences every gun");

    RunConfig drill;
    drill.game.waves = 0;
    drill.game.seconds = 60.0;
    drill.script = {{1.0, "rally E"}, {1.0, "produce tank"}, {30.0, "drones recall"}};
    const auto drilled = quiet(drill);
    check(distanceToPoint(drilled, "K", wargames::sectorPoint("E", 160.0)) < 40.0, "a human 'rally E' sends the tanks east");
    check(drilled.alive.count("tank") != 0 && drilled.alive.at("tank") == 4,
          "a human 'produce tank' is carried out by the commander agent");
    check(distanceToPoint(drilled, "D", {0.0, 0.0}) < 60.0, "a human 'drones recall' brings the interceptors home");

    RunConfig unaided;
    RunConfig directed;
    directed.script = {{0.0, "priority spitter"}, {60.0, "produce tank"}, {120.0, "rally auto"}};
    const auto lost = quiet(unaided);
    const auto saved = quiet(directed);
    check(!lost.outcome.victory && saved.outcome.victory, "the agents alone lose the bunker; the same battle with a human plan holds it");

    RunConfig bad;
    bad.game.waves = 0;
    bad.game.seconds = 10.0;
    bad.script = {{0.0, "rally nowhere"}, {0.0, "fire everything"}};
    const auto rejected = quiet(bad);
    bool logged = false;
    for (const auto& line : rejected.log) logged = logged || line.find("error:") != std::string::npos;
    check(logged, "invalid human directives are rejected with an explanation");

    // Every model call is its own HTTP connection, and each closed connection holds a local
    // port for a while. A shorter battle keeps the three engines' calls well inside the
    // ephemeral port range of every OS (macOS has about 16k ports, Linux about 28k).
    RunConfig brief = base;
    brief.game.seconds = 40.0;
    brief.game.waves = 1;
    const auto local = quiet(brief);
    DoctrineServer llm;
    check(llm.started(), "a loopback chat-API model server started");
    for (const char* flavour : {"ollama", "llama-cpp", "lmstudio"}) {
        RunConfig remote = brief;
        remote.engine = std::string(flavour) + "@127.0.0.1:" + std::to_string(llm.port());
        remote.model = "doctrine";
        const auto before = llm.requests();
        const auto served = quiet(remote);
        const bool same = served.digest == local.digest && served.toolCalls == local.toolCalls && served.failedRuns == 0;
        check(same, same ? std::format("{} engine: {} model calls over HTTP produce the identical battle", flavour, llm.requests() - before)
                         : std::format("{} engine: {} model calls over HTTP, {} failed runs, {} vs {} tool calls", flavour,
                                       llm.requests() - before, served.failedRuns, served.toolCalls, local.toolCalls));
    }

    if (!selfPath.empty()) {
        std::string error;
        auto mod = gygax::service::connectMcp("war=" + selfPath + " --mcp-serve", &error);
        check(mod.has_value(), "the war.* tools are served over MCP (" + error + ")");
        if (mod) {
            const gygax::service::ExternalTool* situation = nullptr;
            for (const auto& t : mod->tools)
                if (t.name == "war_situation") situation = &t;
            check(mod->tools.size() >= 8 && situation != nullptr, std::format("an MCP client lists {} war tools", mod->tools.size()));
            if (situation != nullptr) {
                Battlefield reference{wargames::Options{}};
                const auto viaMcp = gygax::json::parse(situation->invoke("{}"));
                const auto direct = gygax::json::parse(reference.commanderView().dump());
                check(viaMcp && direct && viaMcp->dump() == direct->dump(), "war.situation over MCP matches the in-process battlefield");
            }
        }
    }

    std::cout << "\n" << (failures == 0 ? "WARGAMES OK" : "WARGAMES FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}

}

int main(int argc, char** argv) {
    gygax::log::setLevel(gygax::log::Level::Error);
    auto args = parseArgs(argc, argv);
    if (!args.error.empty()) {
        std::cerr << "wargames: " << args.error << "\n";
        return 2;
    }
    if (args.help) {
        printUsage();
        return 0;
    }
    if (args.mcpServe) return serveMcp();
    if (args.selfcheck) {
        char resolved[4096];
        const ssize_t n = ::readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
        if (n > 0) selfPath.assign(resolved, static_cast<std::size_t>(n));
        return selfcheck();
    }
    if (!args.config.quiet) {
        std::cout << "WARGAMES: last stand. Gygax agents command every defender; you may direct them.\n";
    }
    Game game(args.config);
    const auto result = game.run();
    printReport(result);
    std::string htmlError;
    if (!game.writeHtml(htmlError)) {
        std::cerr << "wargames: " << htmlError << "\n";
        return 1;
    }
    if (!args.config.htmlPath.empty()) std::cout << "graphical replay written to " << args.config.htmlPath << "\n";
    if (!args.config.jsonPath.empty()) {
        std::ofstream out(args.config.jsonPath);
        out << toJson(result).dump(2) << "\n";
    }
    return 0;
}
