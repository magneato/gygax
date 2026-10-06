#include <gygax/service/service.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <sstream>
#include <thread>

#include <gygax/core/log.hpp>
#include <gygax/net/http.hpp>
#include <gygax/neuro/network.hpp>
#include <gygax/logistics/ledger.hpp>
#include <gygax/robotics/device.hpp>
#include <gygax/service/expression.hpp>
#include <gygax/service/logistics_api.hpp>
#include <gygax/service/node_info.hpp>
#include <gygax/service/mcp.hpp>
#include <gygax/service/plugins.hpp>
#include <gygax/version.hpp>

import gygax.orchestration;
import gygax.tools;
import gygax.brain.hierarchy;
import gygax.world_model;

namespace gygax::service {

namespace {

constexpr std::string_view kAgentModel = "gygax-agent";
constexpr std::int64_t kMaximumSessionId = 0xFFFFFFFFLL;
constexpr std::size_t kMaximumMessagesPerRequest = 2000;
constexpr std::size_t kMaximumObjectiveBytes = 64 * 1024;
constexpr std::int64_t kDefaultAgentLogTailLines = 100;
constexpr std::int64_t kMaximumAgentLogTailLines = 4096;
constexpr std::uint32_t kMaximumAgentSteps = 64;
constexpr std::size_t kMaximumRateLimitWindowEntries = 4096;
constexpr double kMaximumWaitSeconds = 300.0;
constexpr std::int64_t kMillisecondsPerSecond = 1000;
constexpr double kMaximumSamplingTemperature = 2.0;
constexpr std::int64_t kMaximumCompletionTokens = 1'000'000;
constexpr int kDefaultHttpErrorStatus = net::kHttpStatusInternalServerError;

struct Result {
    int status = net::kHttpStatusOk;
    json::Value body = json::Value::object();
};

using Params = json::Value;
using OpFn = std::function<Result(Params&)>;

std::int64_t epochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string isoNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

Result fail(int status, std::string_view code, std::string_view message) {
    Result r;
    r.status = status;
    r.body = json::Value::object();
    r.body["error"]["code"] = std::string(code);
    r.body["error"]["message"] = std::string(message);
    r.body["error"]["status"] = status;
    return r;
}

Result okResult(json::Value body, int status = net::kHttpStatusOk) {
    Result r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

net::Response toResponse(const Result& r) {
    return net::Response::json(r.status, r.body.dump());
}

std::optional<std::uint32_t> parseSid(const Params& p) {
    const auto* v = p.find("sid");
    if (v == nullptr) v = p.find("id");
    if (v == nullptr) return std::nullopt;
    if (v->isInt()) {
        const auto n = v->asInt();
        if (n < 0 || n > kMaximumSessionId) return std::nullopt;
        return static_cast<std::uint32_t>(n);
    }
    if (!v->isString()) return std::nullopt;
    const auto& s = v->asString();
    std::uint32_t out = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
    return out;
}

json::Value toJson(const inference::AgentSnapshot& a) {
    json::Value v = json::Value::object();
    v["id"] = a.sid;
    v["name"] = a.name;
    v["status"] = a.status;
    v["objective"] = a.objective;
    v["answer"] = a.answer;
    v["error"] = a.error;
    v["model"] = a.model;
    v["steps"] = a.steps;
    v["max_steps"] = a.maxSteps;
    v["run_id"] = a.runId;
    v["memory_entries"] = a.memoryEntries;
    v["created_ms"] = a.createdMs;
    v["updated_ms"] = a.updatedMs;
    return v;
}

json::Value toJson(const inference::EngineStatus& e) {
    json::Value v = json::Value::object();
    v["id"] = e.id;
    v["kind"] = e.kind;
    v["endpoint"] = e.endpoint;
    v["remote"] = e.remote;
    v["healthy"] = e.healthy;
    v["enabled"] = e.enabled;
    v["pending"] = e.pending;
    v["completed"] = e.completed;
    v["failed"] = e.failed;
    v["consecutive_failures"] = e.consecutiveFailures;
    v["latency_ewma_ms"] = e.latencyEwmaMs;
    v["last_error"] = e.lastError;
    json::Value models = json::Value::array();
    for (const auto& m : e.models) models.push(m);
    v["models"] = std::move(models);
    return v;
}

std::string escapeLabel(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::string stripPort(const std::string& remote) {
    const auto colon = remote.rfind(':');
    return colon == std::string::npos ? remote : remote.substr(0, colon);
}

std::string messageText(const json::Value& content) {
    if (content.isString()) return content.asString();
    std::string out;
    for (const auto& part : content.asArray()) {
        if (part.isString())
            out += part.asString();
        else if (part.getString("type", "text") == "text")
            out += part.getString("text");
    }
    return out;
}

std::optional<std::vector<inference::Message>> parseMessages(const json::Value& body, std::string& error) {
    const auto* arr = body.find("messages");
    if (arr == nullptr || !arr->isArray() || arr->asArray().empty()) {
        error = "'messages' must be a non-empty array";
        return std::nullopt;
    }
    if (arr->asArray().size() > kMaximumMessagesPerRequest) {
        error = "too many messages";
        return std::nullopt;
    }
    std::vector<inference::Message> out;
    for (const auto& m : arr->asArray()) {
        const auto role = m.getString("role");
        if (role != "system" && role != "user" && role != "assistant" && role != "tool" && role != "developer") {
            error = "unsupported message role '" + role + "'";
            return std::nullopt;
        }
        const auto* content = m.find("content");
        out.push_back({role == "developer" ? "system" : role, content == nullptr ? std::string() : messageText(*content)});
    }
    return out;
}

double clampedTemperature(const json::Value& body) {
    return std::clamp(body.getDouble("temperature", inference::kDefaultChatTemperature), 0.0, kMaximumSamplingTemperature);
}

int clampedMaxTokens(const json::Value& body) {
    std::int64_t n = body.getInt("max_tokens", body.getInt("max_completion_tokens", 0));
    return static_cast<int>(std::clamp<std::int64_t>(n, 0, kMaximumCompletionTokens));
}

}

std::string readTokenFile(const std::string& path) {
    std::ifstream in(path);
    std::string token;
    if (in) std::getline(in, token);
    while (!token.empty() && (token.back() == '\r' || token.back() == ' ' || token.back() == '\t')) token.pop_back();
    return token;
}

bool isLoopbackHost(const std::string& host) {
    return host == "localhost" || host == "::1" || host == "[::1]" || host.starts_with("127.");
}

ServiceConfig ServiceConfig::fromEnvironment() {
    ServiceConfig c;
    auto get = [](const char* name) -> std::string {
        const char* v = std::getenv(name);
        return v == nullptr ? std::string() : std::string(v);
    };
    auto split = [](const std::string& text) {
        std::vector<std::string> out;
        std::istringstream in(text);
        std::string item;
        while (std::getline(in, item, ',')) {
            while (!item.empty() && item.front() == ' ') item.erase(item.begin());
            while (!item.empty() && item.back() == ' ') item.pop_back();
            if (!item.empty()) out.push_back(item);
        }
        return out;
    };
    if (auto v = get("GYGAX_HOST"); !v.empty()) c.host = v;
    if (auto v = get("GYGAX_PORT"); !v.empty()) c.port = static_cast<std::uint16_t>(std::strtoul(v.c_str(), nullptr, 10));
    c.token = get("GYGAX_API_TOKEN");
    if (c.token.empty()) {
        if (auto file = get("GYGAX_API_TOKEN_FILE"); !file.empty()) c.token = readTokenFile(file);
    }
    c.peerToken = get("GYGAX_PEER_TOKEN");
    c.engines = split(get("GYGAX_ENGINES"));
    c.peers = split(get("GYGAX_PEERS"));
    c.defaultModel = get("GYGAX_MODEL");
    if (auto v = get("GYGAX_TOOL_PROTOCOL"); !v.empty()) c.toolProtocol = v;
    c.corsOrigin = get("GYGAX_CORS_ORIGIN");
    c.allowInsecureRemote = get("GYGAX_ALLOW_INSECURE_REMOTE") == "1";
    c.enableDeviceCommands = get("GYGAX_DEVICE_COMMANDS") == "1";
    c.ledgerPath = get("GYGAX_LEDGER_PATH");
    c.plugins = split(get("GYGAX_PLUGINS"));
    c.extensions = split(get("GYGAX_EXTENSIONS"));
    c.mcpServers = split(get("GYGAX_MCP"));
    if (auto v = get("GYGAX_RATE_LIMIT_PER_MINUTE"); !v.empty()) c.rateLimitPerMinute = std::strtoul(v.c_str(), nullptr, 10);
    if (auto v = get("GYGAX_HTTP_WORKERS"); !v.empty()) c.httpWorkers = std::strtoul(v.c_str(), nullptr, 10);
    if (auto v = get("GYGAX_AGENT_WORKERS"); !v.empty()) c.agentWorkers = std::strtoul(v.c_str(), nullptr, 10);
    if (auto v = get("GYGAX_AGENT_TIMEOUT_MS"); !v.empty()) c.agentTimeoutMs = std::strtoll(v.c_str(), nullptr, 10);
    return c;
}

bool ServiceConfig::validate(std::string& error) const {
    if (enableHttp && !isLoopbackHost(host) && token.empty() && !allowInsecureRemote) {
        error = "refusing to listen on '" + host + "' without GYGAX_API_TOKEN; set a token or GYGAX_ALLOW_INSECURE_REMOTE=1";
        return false;
    }
    if (httpWorkers == 0 || agentWorkers == 0) {
        error = "worker counts must be positive";
        return false;
    }
    if (maxBodyBytes < 1024) {
        error = "maxBodyBytes is unreasonably small";
        return false;
    }
    if (toolProtocol != "json" && toolProtocol != "atari") {
        error = "toolProtocol must be 'json' or 'atari', got '" + toolProtocol + "'";
        return false;
    }
    for (const auto& spec : engines) {
        std::string err;
        const std::string resolved = inference::resolveEngineSpec(spec);
        if (spec != "echo" && !net::Url::parse(resolved.substr(0, resolved.find(';')), &err)) {
            error = "invalid engine spec '" + spec + "': " + err;
            return false;
        }
    }
    for (const auto& peer : peers) {
        std::string err;
        if (!net::Url::parse(peer, &err)) {
            error = "invalid peer URL '" + peer + "': " + err;
            return false;
        }
    }
    return true;
}

struct Service::Impl {
    struct Op {
        std::string rpcName;
        std::string method;
        std::string pattern;
        OpFn fn;
    };

    ServiceConfig config;
    std::shared_ptr<inference::EnginePool> pool;
    std::shared_ptr<inference::PoolBackend> poolBackend;
    std::unique_ptr<inference::Orchestrator> orchestrator;
    std::unique_ptr<net::Server> server;
    std::vector<Op> ops;
    std::map<std::string, Op*> rpcTable;
    std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> completionCounter{0};
    std::atomic<std::uint64_t> chatRequests{0};
    std::atomic<std::uint64_t> chatErrors{0};
    std::atomic<std::uint64_t> agentChatRequests{0};
    std::atomic<bool> running{false};
    std::vector<std::string> ownedTools;
    robotics::DeviceRegistry devices;
    std::unique_ptr<logistics::Ledger> ledger;
    std::vector<ExternalModule> modules;

    std::mutex layoutMutex;
    std::string latestHtml = "<h1>No layout generated yet</h1>";
    std::string latestText = "Send a directive to generate a layout.";
    std::optional<std::uint32_t> portalAgent;

    std::mutex rateMutex;
    struct Window {
        std::chrono::steady_clock::time_point start;
        std::size_t count = 0;
    };
    std::map<std::string, Window> windows;

    explicit Impl(ServiceConfig c) : config(std::move(c)) {}

    void registerBuiltinTools() {
        auto& reg = tools::ToolRegistry::getInstance();
        ownedTools = {"time.now",     "math.eval",        "node.info",         "neuro.run",      "webpage.construct", "device.list",
                      "device.state", "logistics.record", "logistics.summary", "logistics.plan", "logistics.units"};
        if (config.enableDeviceCommands) ownedTools.emplace_back("device.command");
        reg.registerTool("time.now", [](const std::string&) { return isoNow(); }, "Current UTC time in ISO-8601");
        reg.registerTool(
            "math.eval",
            [](const std::string& input) {
                double value = 0.0;
                std::string error;
                if (!evaluateExpression(input, value, error)) throw std::runtime_error(error);
                return json::Value(value).dump();
            },
            "Evaluate an arithmetic expression: + - * / % ^, parentheses, sqrt sin cos tan exp ln abs min max pow");
        reg.registerTool(
            "node.info", [](const std::string&) { return toJson(collectNodeInfo()).dump(); },
            "Hardware and load information for this node");
        reg.registerTool(
            "neuro.run",
            [](const std::string& input) {
                std::string error;
                auto spec = json::parse(input, &error);
                if (!spec) throw std::runtime_error("invalid spec JSON: " + error);
                auto out = neuro::simulateSpec(*spec, &error);
                if (out.isNull()) throw std::runtime_error(error);
                return out.dump();
            },
            "Run a spiking neural network simulation from a JSON spec (populations, projections, run_ms) and return spike statistics");
        reg.registerTool(
            "webpage.construct",
            [this](const std::string&) {
                auto [html, text] = brain::CoordinateWebPage();
                std::lock_guard lock(layoutMutex);
                latestHtml = html;
                latestText = text;
                return std::format("layout constructed ({} bytes)", html.size());
            },
            "Construct the portal web page with the Mother/Father/Child agent hierarchy");
        reg.registerTool(
            "device.list", [this](const std::string&) { return devices.list().dump(); },
            "List connected robots, vehicles, buses and instruments with their kind, endpoint, connection state and supported commands");
        reg.registerTool(
            "device.state",
            [this](const std::string& input) {
                std::string id = input;
                if (auto parsed = json::parse(input); parsed && parsed->isObject()) id = parsed->getString("device");
                auto device = devices.get(id);
                if (!device) throw std::runtime_error("no such device '" + id + "'");
                return device->state().dump();
            },
            "Read the latest telemetry of a device. Input: the device id or {\"device\": id}");
        auto logisticsTool = [this](const char* name, const char* op, const char* description) {
            tools::ToolRegistry::getInstance().registerTool(
                name,
                [this, op](const std::string& input) {
                    json::Value params = json::Value::object();
                    if (auto parsed = json::parse(input); parsed && parsed->isObject()) {
                        params = std::move(*parsed);
                    } else if (std::string_view(op) == "events.add") {
                        params["line"] = input;
                    } else if (std::string_view(op) == "summary" && !input.empty()) {
                        params["since"] = input;
                    }
                    auto result = handleLogistics(*ledger, devices, op, params);
                    if (result.status >= net::kHttpStatusBadRequest)
                        throw std::runtime_error(result.body.find("error")->getString("message"));
                    return result.body.dump();
                },
                description);
        };
        logisticsTool("logistics.record", "events.add",
                      "Record inventory flow. Input: a line like \"-3 ba99x drone=quadcopter power=solar\" (negative loses units, positive "
                      "produces them) or JSON {delta, sku, attrs, guids, reason, site, note}");
        logisticsTool(
            "logistics.summary", "summary",
            "Fleet and inventory report with active, lost and retired units per sku and the flow lines for a window. Input: optional "
            "window such as 7d");
        logisticsTool(
            "logistics.plan", "plan",
            "Range-aware route plan that inserts refuel stops. Input JSON: {unit or sku, origin, waypoints:[{latitude,longitude}], "
            "return_home, max_range_m, fuel}");
        logisticsTool("logistics.units", "units.list", "List tracked units. Input JSON: {sku, status: active|lost|retired, limit}");
        if (config.enableDeviceCommands) {
            reg.registerTool(
                "device.command",
                [this](const std::string& input) {
                    auto parsed = json::parse(input);
                    if (!parsed || !parsed->isObject()) throw std::runtime_error("input must be a JSON object with device and command");
                    auto device = devices.get(parsed->getString("device"));
                    if (!device) throw std::runtime_error("no such device");
                    auto result = device->command(*parsed);
                    if (result.status >= net::kHttpStatusBadRequest)
                        throw std::runtime_error(result.body.find("error") != nullptr ? result.body.find("error")->getString("message")
                                                                                      : "command failed");
                    return result.body.dump();
                },
                "Send a command to a device. Input: {\"device\": id, \"command\": name, ...fields}. Physical devices move; only use when "
                "the objective requires it");
        }
    }

    void adoptModule(ExternalModule&& mod) {
        auto& reg = tools::ToolRegistry::getInstance();
        for (const auto& tool : mod.tools) {
            const std::string name = mod.kind + "." + mod.name + "." + tool.name;
            if (reg.has(name)) throw std::runtime_error("tool '" + name + "' is already registered");
            reg.registerTool(name, tool.invoke, tool.description);
            ownedTools.push_back(name);
        }
        log::info("service", "loaded {} '{}' {} with {} tools", mod.kind, mod.name, mod.version, mod.tools.size());
        modules.push_back(std::move(mod));
    }

    void loadExternalModules() {
        for (const auto& path : config.plugins) {
            std::string error;
            auto mod = loadPlugin(path, &error);
            if (!mod) throw std::runtime_error(error);
            adoptModule(std::move(*mod));
        }
        for (const auto& spec : config.extensions) {
            std::string error;
            auto mod = connectExtension(spec, &error);
            if (!mod) throw std::runtime_error(error);
            adoptModule(std::move(*mod));
        }
        for (const auto& spec : config.mcpServers) {
            std::string error;
            auto mod = connectMcp(spec, &error);
            if (!mod) throw std::runtime_error(error);
            adoptModule(std::move(*mod));
        }
    }

    void setupEngines() {
        std::vector<std::string> specs = config.engines;
        if (specs.empty()) specs.emplace_back("echo");
        std::size_t index = 0;
        for (const auto& spec : specs) {
            std::string error;
            auto backend = inference::makeBackend(spec, &error);
            if (!backend) throw std::runtime_error(error);
            std::string id = spec == "echo" ? "echo" : std::format("engine-{}", ++index);
            if (!pool->addEngine(id, backend, &error)) throw std::runtime_error(error);
        }
        const std::string peerKey = config.peerToken.empty() ? config.token : config.peerToken;
        for (const auto& peer : config.peers) {
            std::string base = peer;
            while (base.size() > 1 && base.back() == '/') base.pop_back();
            std::string spec = base + "/v1;peer=1";
            if (!peerKey.empty()) spec += ";key=" + peerKey;
            std::string error;
            auto backend = inference::makeBackend(spec, &error);
            if (!backend) throw std::runtime_error(error);
            auto url = net::Url::parse(base);
            if (!pool->addEngine("peer:" + url->authority(), backend, &error)) throw std::runtime_error(error);
        }
    }

    json::Value nodeJson() {
        json::Value out = toJson(collectNodeInfo());
        out["version"] = GYGAX_VERSION_STRING;
        out["uptime_service_seconds"] =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startedAt).count();
        out["engines_healthy"] = pool->healthyCount();
        out["engines_total"] = pool->size();
        return out;
    }

    void add(std::string rpcName, std::string method, std::string pattern, OpFn fn) {
        ops.push_back(Op{std::move(rpcName), std::move(method), std::move(pattern), std::move(fn)});
    }

    void defineOps() {
        add("node.info", "GET", "/v1/node", [this](Params&) { return okResult(nodeJson()); });

        add("cluster.info", "GET", "/v1/cluster", [this](Params&) {
            json::Value out = json::Value::object();
            out["self"] = nodeJson();
            json::Value peers = json::Value::array();
            for (const auto& e : pool->status()) {
                if (!e.remote) continue;
                peers.push(toJson(e));
            }
            out["peers"] = std::move(peers);
            return okResult(std::move(out));
        });

        add("engines.list", "GET", "/v1/engines", [this](Params&) {
            json::Value out = json::Value::object();
            json::Value list = json::Value::array();
            for (const auto& e : pool->status()) list.push(toJson(e));
            out["engines"] = std::move(list);
            if (auto pinned = pool->pinned()) out["pinned"] = *pinned;
            return okResult(std::move(out));
        });

        add("engines.add", "POST", "/v1/engines", [this](Params& p) {
            const auto id = p.getString("id");
            const auto spec = p.getString("spec");
            if (id.empty() || spec.empty())
                return fail(net::kHttpStatusBadRequest, "invalid_request", "'id' and 'spec' are required");
            std::string error;
            auto backend = inference::makeBackend(spec, &error);
            if (!backend) return fail(net::kHttpStatusBadRequest, "invalid_engine", error);
            if (!pool->addEngine(id, backend, &error)) return fail(net::kHttpStatusConflict, "engine_exists", error);
            pool->probeNow();
            json::Value out = json::Value::object();
            out["id"] = id;
            return okResult(std::move(out), net::kHttpStatusCreated);
        });

        add("engines.remove", "DELETE", "/v1/engines/:id", [this](Params& p) {
            if (!pool->removeEngine(p.getString("id"))) return fail(net::kHttpStatusNotFound, "not_found", "no such engine");
            return okResult(json::Value::object());
        });

        add("engines.enable", "POST", "/v1/engines/:id/enable", [this](Params& p) {
            if (!pool->setEnabled(p.getString("id"), true)) return fail(net::kHttpStatusNotFound, "not_found", "no such engine");
            return okResult(json::Value::object());
        });

        add("engines.disable", "POST", "/v1/engines/:id/disable", [this](Params& p) {
            if (!pool->setEnabled(p.getString("id"), false)) return fail(net::kHttpStatusNotFound, "not_found", "no such engine");
            return okResult(json::Value::object());
        });

        add("engines.pin", "POST", "/v1/pin", [this](Params& p) {
            const auto id = p.getString("id");
            if (id.empty()) {
                pool->pin(std::nullopt);
            } else {
                bool found = false;
                for (const auto& e : pool->status()) found = found || e.id == id;
                if (!found) return fail(net::kHttpStatusNotFound, "not_found", "no such engine");
                pool->pin(id);
            }
            return okResult(json::Value::object());
        });

        add("engines.probe", "POST", "/v1/probe", [this](Params&) {
            pool->probeNow();
            return okResult(json::Value::object());
        });

        add("tools.list", "GET", "/v1/tools", [](Params&) {
            json::Value list = json::Value::array();
            for (const auto& t : tools::ToolRegistry::getInstance().list()) {
                json::Value item = json::Value::object();
                item["name"] = t.name;
                item["description"] = t.description;
                list.push(std::move(item));
            }
            json::Value out = json::Value::object();
            out["tools"] = std::move(list);
            return okResult(std::move(out));
        });

        add("tools.invoke", "POST", "/v1/tools/:name/invoke", [](Params& p) {
            const auto name = p.getString("name");
            const auto* input = p.find("input");
            const std::string text = input == nullptr ? std::string() : (input->isString() ? input->asString() : input->dump());
            try {
                auto output = tools::ToolRegistry::getInstance().execute(name, text);
                if (!output) return fail(net::kHttpStatusNotFound, "not_found", "no such tool");
                json::Value out = json::Value::object();
                out["tool"] = name;
                out["output"] = *output;
                return okResult(std::move(out));
            } catch (const std::exception& e) {
                return fail(net::kHttpStatusUnprocessableEntity, "tool_error", e.what());
            }
        });

        add("agents.list", "GET", "/v1/agents", [this](Params&) {
            json::Value list = json::Value::array();
            for (const auto& a : orchestrator->agents()) list.push(toJson(a));
            json::Value out = json::Value::object();
            out["agents"] = std::move(list);
            return okResult(std::move(out));
        });

        add("agents.create", "POST", "/v1/agents", [this](Params& p) {
            const auto steps =
                static_cast<std::uint32_t>(std::clamp<std::int64_t>(p.getInt("max_steps", 0), 0, kMaximumAgentSteps));
            const auto sid = orchestrator->createAgent(p.getString("name"), steps);
            auto snap = orchestrator->inspect(sid);
            return okResult(toJson(*snap), net::kHttpStatusCreated);
        });

        add("agents.get", "GET", "/v1/agents/:sid", [this](Params& p) {
            const auto sid = parseSid(p);
            if (!sid) return fail(net::kHttpStatusBadRequest, "invalid_request", "invalid agent id");
            auto snap = orchestrator->inspect(*sid);
            if (!snap) return fail(net::kHttpStatusNotFound, "not_found", "no such agent");
            return okResult(toJson(*snap));
        });

        add("agents.delete", "DELETE", "/v1/agents/:sid", [this](Params& p) {
            const auto sid = parseSid(p);
            if (!sid) return fail(net::kHttpStatusBadRequest, "invalid_request", "invalid agent id");
            if (!orchestrator->removeAgent(*sid)) return fail(net::kHttpStatusNotFound, "not_found", "no such agent");
            return okResult(json::Value::object());
        });

        add("agents.submit", "POST", "/v1/agents/:sid/objectives", [this](Params& p) {
            const auto sid = parseSid(p);
            if (!sid) return fail(net::kHttpStatusBadRequest, "invalid_request", "invalid agent id");
            const auto objective = p.getString("objective");
            if (objective.empty()) return fail(net::kHttpStatusBadRequest, "invalid_request", "'objective' is required");
            if (objective.size() > kMaximumObjectiveBytes)
                return fail(net::kHttpStatusPayloadTooLarge, "payload_too_large", "objective too long");
            switch (orchestrator->submit(*sid, objective, p.getString("model"))) {
            case inference::SubmitResult::UnknownAgent: return fail(net::kHttpStatusNotFound, "not_found", "no such agent");
            case inference::SubmitResult::Busy:
                return fail(net::kHttpStatusConflict, "busy", "agent is already running an objective");
            case inference::SubmitResult::Stopped:
                return fail(net::kHttpStatusServiceUnavailable, "unavailable", "orchestrator is stopped");
            case inference::SubmitResult::Invalid:
                return fail(net::kHttpStatusBadRequest, "invalid_request", "invalid objective");
            case inference::SubmitResult::Accepted: break;
            }
            const auto waitSeconds = std::clamp(p.getDouble("wait_seconds", 0.0), 0.0, kMaximumWaitSeconds);
            std::optional<inference::AgentSnapshot> snap;
            if (waitSeconds > 0.0)
                snap = orchestrator->wait(
                    *sid, std::chrono::milliseconds(static_cast<std::int64_t>(waitSeconds * kMillisecondsPerSecond)));
            else
                snap = orchestrator->inspect(*sid);
            const bool finished = snap && snap->status != "running";
            return okResult(toJson(*snap), finished ? net::kHttpStatusOk : net::kHttpStatusAccepted);
        });

        add("agents.cancel", "POST", "/v1/agents/:sid/cancel", [this](Params& p) {
            const auto sid = parseSid(p);
            if (!sid) return fail(net::kHttpStatusBadRequest, "invalid_request", "invalid agent id");
            if (!orchestrator->inspect(*sid)) return fail(net::kHttpStatusNotFound, "not_found", "no such agent");
            if (!orchestrator->cancel(*sid)) return fail(net::kHttpStatusConflict, "not_running", "agent is not running");
            return okResult(json::Value::object(), net::kHttpStatusAccepted);
        });

        add("agents.memory", "GET", "/v1/agents/:sid/memory", [this](Params& p) {
            const auto sid = parseSid(p);
            if (!sid) return fail(net::kHttpStatusBadRequest, "invalid_request", "invalid agent id");
            if (!orchestrator->inspect(*sid)) return fail(net::kHttpStatusNotFound, "not_found", "no such agent");
            std::int64_t tail = kDefaultAgentLogTailLines;
            if (const auto* t = p.find("tail")) {
                if (t->isInt())
                    tail = t->asInt();
                else if (t->isString())
                    std::from_chars(t->asString().data(), t->asString().data() + t->asString().size(), tail);
            }
            json::Value entries = json::Value::array();
            for (const auto& e :
                 orchestrator->memory(*sid, static_cast<std::size_t>(std::clamp<std::int64_t>(tail, 1, kMaximumAgentLogTailLines))))
                entries.push(e);
            json::Value out = json::Value::object();
            out["entries"] = std::move(entries);
            return okResult(std::move(out));
        });

        add("devices.list", "GET", "/v1/devices", [this](Params&) {
            json::Value out = json::Value::object();
            out["devices"] = devices.list();
            json::Value kinds = json::Value::array();
            for (const auto& k : robotics::deviceKinds()) kinds.push(k);
            out["kinds"] = std::move(kinds);
            out["commands_enabled"] = config.enableDeviceCommands;
            return okResult(std::move(out));
        });

        add("devices.add", "POST", "/v1/devices", [this](Params& p) {
            const auto id = p.getString("id");
            std::string error;
            auto device = robotics::openDevice(p, &error);
            if (!device) return fail(net::kHttpStatusBadRequest, "invalid_device", error);
            if (!devices.add(id, std::move(device), &error))
                return fail(error.find("exists") != std::string::npos ? net::kHttpStatusConflict : net::kHttpStatusBadRequest,
                            "invalid_device", error);
            json::Value out = json::Value::object();
            out["id"] = id;
            return okResult(std::move(out), net::kHttpStatusCreated);
        });

        add("devices.remove", "DELETE", "/v1/devices/:device", [this](Params& p) {
            if (!devices.remove(p.getString("device"))) return fail(net::kHttpStatusNotFound, "not_found", "no such device");
            return okResult(json::Value::object());
        });

        add("devices.state", "GET", "/v1/devices/:device/state", [this](Params& p) {
            auto device = devices.get(p.getString("device"));
            if (!device) return fail(net::kHttpStatusNotFound, "not_found", "no such device");
            json::Value out = json::Value::object();
            out["kind"] = device->kind();
            out["connected"] = device->connected();
            out["state"] = device->state();
            return okResult(std::move(out));
        });

        add("devices.command", "POST", "/v1/devices/:device/command", [this](Params& p) {
            if (!config.enableDeviceCommands) {
                return fail(net::kHttpStatusForbidden, "commands_disabled",
                            "device commands are disabled; start the service with GYGAX_DEVICE_COMMANDS=1 to allow them");
            }
            auto device = devices.get(p.getString("device"));
            if (!device) return fail(net::kHttpStatusNotFound, "not_found", "no such device");
            auto result = device->command(p);
            return Result{result.status, std::move(result.body)};
        });

        const struct {
            const char* rpc;
            const char* method;
            const char* path;
            const char* op;
        } logisticsRoutes[] = {
            {"logistics.models.put", "POST", "/v1/logistics/models", "models.put"},
            {"logistics.models.list", "GET", "/v1/logistics/models", "models.list"},
            {"logistics.sites.put", "POST", "/v1/logistics/sites", "sites.put"},
            {"logistics.sites.list", "GET", "/v1/logistics/sites", "sites.list"},
            {"logistics.sites.remove", "DELETE", "/v1/logistics/sites/:site", "sites.remove"},
            {"logistics.events.add", "POST", "/v1/logistics/events", "events.add"},
            {"logistics.events.list", "GET", "/v1/logistics/events", "events.list"},
            {"logistics.units.list", "GET", "/v1/logistics/units", "units.list"},
            {"logistics.units.get", "GET", "/v1/logistics/units/:guid", "units.get"},
            {"logistics.units.update", "POST", "/v1/logistics/units/:guid", "units.update"},
            {"logistics.summary", "GET", "/v1/logistics/summary", "summary"},
            {"logistics.plan", "POST", "/v1/logistics/plan", "plan"},
        };
        for (const auto& route : logisticsRoutes) {
            const std::string op = route.op;
            add(route.rpc, route.method, route.path, [this, op](Params& p) {
                auto result = handleLogistics(*ledger, devices, op, p);
                return Result{result.status, std::move(result.body)};
            });
        }

        add("neuro.simulate", "POST", "/v1/neuro/simulate", [](Params& p) {
            std::string error;
            auto out = neuro::simulateSpec(p, &error);
            if (out.isNull()) return fail(net::kHttpStatusBadRequest, "invalid_spec", error);
            return okResult(std::move(out));
        });

        add("chat.complete", "POST", "/v1/chat/completions.json", [this](Params& p) { return chatOnce(p, false); });

        for (auto& op : ops) rpcTable[op.rpcName] = &op;
    }

    inference::ChatResult runAgentChat(const std::string& innerModel, const std::vector<inference::Message>& messages, std::string& error,
                                       int& status) {
        inference::ChatResult result;
        std::string objective;
        if (messages.size() == 1) {
            objective = messages.front().content;
        } else {
            for (const auto& m : messages) objective += m.role + ": " + m.content + "\n";
        }
        if (objective.empty()) {
            status = net::kHttpStatusBadRequest;
            error = "empty objective";
            return result;
        }
        const auto sid = orchestrator->createAgent(std::format("chat-{}", ++agentChatRequests), config.agentMaxSteps);
        const auto submitted = orchestrator->submit(sid, objective, innerModel);
        if (submitted != inference::SubmitResult::Accepted) {
            orchestrator->removeAgent(sid);
            status = net::kHttpStatusServiceUnavailable;
            error = "agent runtime unavailable";
            return result;
        }
        auto snap = orchestrator->wait(sid, std::chrono::milliseconds(config.agentTimeoutMs));
        if (!snap || snap->status == "running") {
            orchestrator->cancel(sid);
            orchestrator->removeAgent(sid);
            status = net::kHttpStatusGatewayTimeout;
            error = "agent did not finish before the timeout";
            return result;
        }
        orchestrator->removeAgent(sid);
        if (snap->status != "completed") {
            status = net::kHttpStatusBadGateway;
            error = snap->error.empty() ? "agent failed" : snap->error;
            return result;
        }
        result.ok = true;
        result.status = net::kHttpStatusOk;
        result.text = snap->answer;
        result.model = std::string(kAgentModel);
        return result;
    }

    struct ChatContext {
        inference::ChatRequest request;
        bool stream = false;
        bool agent = false;
        std::string agentInner;
        bool forwarded = false;
    };

    std::optional<Result> buildChat(const Params& body, bool forwarded, ChatContext& ctx, bool ollama = false) {
        std::string error;
        auto messages = parseMessages(body, error);
        if (!messages) return fail(net::kHttpStatusBadRequest, "invalid_request", error);
        ctx.request.messages = std::move(*messages);
        std::string model = body.getString("model", config.defaultModel);
        ctx.request.temperature = clampedTemperature(body);
        ctx.request.maxTokens = clampedMaxTokens(body);
        ctx.request.localOnly = forwarded;
        ctx.forwarded = forwarded;
        ctx.stream = body.getBool("stream", ollama);
        if (model == kAgentModel || model.starts_with(std::string(kAgentModel) + ":")) {
            if (forwarded)
                return fail(net::kHttpStatusBadRequest, "invalid_request", "agent mode is not available for forwarded requests");
            ctx.agent = true;
            ctx.agentInner = model.size() > kAgentModel.size() ? model.substr(kAgentModel.size() + 1) : config.defaultModel;
        }
        ctx.request.model = model;
        return std::nullopt;
    }

    static json::Value completionJson(const std::string& id, const std::string& model, const inference::ChatResult& r) {
        json::Value out = json::Value::object();
        out["id"] = id;
        out["object"] = "chat.completion";
        out["created"] = epochSeconds();
        out["model"] = r.model.empty() ? model : r.model;
        json::Value choice = json::Value::object();
        choice["index"] = 0;
        choice["message"]["role"] = "assistant";
        choice["message"]["content"] = r.text;
        choice["finish_reason"] = "stop";
        out["choices"].push(std::move(choice));
        out["usage"]["prompt_tokens"] = r.promptTokens;
        out["usage"]["completion_tokens"] = r.completionTokens;
        out["usage"]["total_tokens"] = r.promptTokens + r.completionTokens;
        out["gygax"]["engine"] = r.engine;
        return out;
    }

    Result chatOnce(Params& body, bool forwarded) {
        ChatContext ctx;
        if (auto err = buildChat(body, forwarded, ctx)) return *err;
        ++chatRequests;
        inference::ChatResult r;
        if (ctx.agent) {
            std::string error;
            int status = kDefaultHttpErrorStatus;
            r = runAgentChat(ctx.agentInner, ctx.request.messages, error, status);
            if (!r.ok) {
                ++chatErrors;
                return fail(status, "agent_error", error);
            }
        } else {
            r = pool->chat(ctx.request);
            if (!r.ok) {
                ++chatErrors;
                return fail(r.status >= net::kHttpStatusBadRequest ? r.status : net::kHttpStatusBadGateway,
                            "engine_error", r.error);
            }
        }
        return okResult(completionJson(std::format("chatcmpl-{}", ++completionCounter), ctx.request.model, r));
    }

    net::Response chatCompletions(net::Request& req) {
        auto parsed = parseJsonBody(req);
        if (!parsed.first) return toResponse(parsed.second);
        Params& body = *parsed.first;
        ChatContext ctx;
        const bool forwarded = req.header("X-Gygax-Forwarded") == "1";
        if (auto err = buildChat(body, forwarded, ctx)) return toResponse(*err);
        if (!ctx.stream) return toResponse(chatOnce(body, forwarded));

        ++chatRequests;
        const std::string id = std::format("chatcmpl-{}", ++completionCounter);
        const std::string modelName = ctx.request.model;
        return net::Response::streaming(net::kHttpStatusOk, "text/event-stream",
                                        [this, ctx = std::move(ctx), id, modelName](net::ChunkWriter& w) mutable {
            auto chunk = [&](const json::Value& delta, std::string_view finish) {
                json::Value c = json::Value::object();
                c["id"] = id;
                c["object"] = "chat.completion.chunk";
                c["created"] = epochSeconds();
                c["model"] = modelName;
                json::Value choice = json::Value::object();
                choice["index"] = 0;
                choice["delta"] = delta;
                choice["finish_reason"] = finish.empty() ? json::Value(nullptr) : json::Value(std::string(finish));
                c["choices"].push(std::move(choice));
                return "data: " + c.dump() + "\n\n";
            };
            auto failWith = [&](int status, const std::string& message) {
                ++chatErrors;
                if (!w.headersSent()) {
                    w.setStatus(status);
                    w.setHeader("Content-Type", "application/json");
                    w.write(fail(status, "engine_error", message).body.dump());
                } else {
                    json::Value e = json::Value::object();
                    e["error"]["message"] = message;
                    w.write("data: " + e.dump() + "\n\n");
                    w.write("data: [DONE]\n\n");
                }
            };
            bool started = false;
            auto emit = [&](std::string_view piece) {
                if (!started) {
                    started = true;
                    json::Value role = json::Value::object();
                    role["role"] = "assistant";
                    role["content"] = "";
                    if (!w.write(chunk(role, ""))) return false;
                }
                json::Value delta = json::Value::object();
                delta["content"] = std::string(piece);
                return w.write(chunk(delta, ""));
            };
            if (ctx.agent) {
                std::string error;
                int status = kDefaultHttpErrorStatus;
                auto r = runAgentChat(ctx.agentInner, ctx.request.messages, error, status);
                if (!r.ok) return failWith(status, error);
                emit(r.text);
            } else {
                auto r = pool->chatStream(ctx.request, emit);
                if (!r.ok)
                    return failWith(r.status >= net::kHttpStatusBadRequest ? r.status : net::kHttpStatusBadGateway, r.error);
            }
            if (!started) emit("");
            w.write(chunk(json::Value::object(), "stop"));
            w.write("data: [DONE]\n\n");
        });
    }

    net::Response listModels(net::Request& req) {
        const bool forwarded = req.header("X-Gygax-Forwarded") == "1";
        std::set<std::string> ids;
        for (const auto& e : pool->status()) {
            if (!e.healthy || !e.enabled || (forwarded && e.remote)) continue;
            if (e.kind == "echo") ids.insert("echo");
            ids.insert(e.models.begin(), e.models.end());
        }
        if (!forwarded) ids.insert(std::string(kAgentModel));
        json::Value data = json::Value::array();
        for (const auto& id : ids) {
            json::Value m = json::Value::object();
            m["id"] = id;
            m["object"] = "model";
            m["created"] = 0;
            m["owned_by"] = id == kAgentModel ? "gygax" : "engine";
            data.push(std::move(m));
        }
        json::Value out = json::Value::object();
        out["object"] = "list";
        out["data"] = std::move(data);
        return net::Response::json(net::kHttpStatusOk, out.dump());
    }

    net::Response ollamaTags(net::Request& req) {
        const bool forwarded = req.header("X-Gygax-Forwarded") == "1";
        std::set<std::string> ids;
        for (const auto& e : pool->status()) {
            if (!e.healthy || !e.enabled || (forwarded && e.remote)) continue;
            if (e.kind == "echo") ids.insert("echo");
            ids.insert(e.models.begin(), e.models.end());
        }
        json::Value models = json::Value::array();
        for (const auto& id : ids) {
            json::Value m = json::Value::object();
            m["name"] = id;
            m["model"] = id;
            models.push(std::move(m));
        }
        json::Value out = json::Value::object();
        out["models"] = std::move(models);
        return net::Response::json(net::kHttpStatusOk, out.dump());
    }

    net::Response ollamaChat(net::Request& req, bool generate) {
        auto parsed = parseJsonBody(req);
        if (!parsed.first) return toResponse(parsed.second);
        Params body = std::move(*parsed.first);
        if (generate) {
            const auto prompt = body.getString("prompt");
            if (prompt.empty()) return toResponse(fail(net::kHttpStatusBadRequest, "invalid_request", "'prompt' is required"));
            json::Value messages = json::Value::array();
            if (const auto system = body.getString("system"); !system.empty()) {
                json::Value m = json::Value::object();
                m["role"] = "system";
                m["content"] = system;
                messages.push(std::move(m));
            }
            json::Value m = json::Value::object();
            m["role"] = "user";
            m["content"] = prompt;
            messages.push(std::move(m));
            body["messages"] = std::move(messages);
        }
        ChatContext ctx;
        const bool forwarded = req.header("X-Gygax-Forwarded") == "1";
        if (auto err = buildChat(body, forwarded, ctx, true)) return toResponse(*err);
        ++chatRequests;
        const std::string modelName = ctx.request.model;

        auto frame = [modelName, generate](std::string_view piece, bool done) {
            json::Value o = json::Value::object();
            o["model"] = modelName;
            o["created_at"] = isoNow();
            if (generate) {
                o["response"] = std::string(piece);
            } else {
                o["message"]["role"] = "assistant";
                o["message"]["content"] = std::string(piece);
            }
            o["done"] = done;
            if (done) o["done_reason"] = "stop";
            return o.dump();
        };

        if (!ctx.stream) {
            inference::ChatResult r;
            if (ctx.agent) {
                std::string error;
                int status = kDefaultHttpErrorStatus;
                r = runAgentChat(ctx.agentInner, ctx.request.messages, error, status);
                if (!r.ok) return toResponse(fail(status, "agent_error", error));
            } else {
                r = pool->chat(ctx.request);
                if (!r.ok) {
                    ++chatErrors;
                    return toResponse(fail(r.status >= net::kHttpStatusBadRequest ? r.status : net::kHttpStatusBadGateway,
                                           "engine_error", r.error));
                }
            }
            return net::Response::json(net::kHttpStatusOk, frame(r.text, true));
        }
        return net::Response::streaming(net::kHttpStatusOk, "application/x-ndjson",
                                        [this, ctx = std::move(ctx), frame](net::ChunkWriter& w) mutable {
            if (ctx.agent) {
                std::string error;
                int status = kDefaultHttpErrorStatus;
                auto r = runAgentChat(ctx.agentInner, ctx.request.messages, error, status);
                if (!r.ok) {
                    w.setStatus(status);
                    w.setHeader("Content-Type", "application/json");
                    w.write(fail(status, "agent_error", error).body.dump());
                    return;
                }
                w.write(frame(r.text, false) + "\n");
                w.write(frame("", true) + "\n");
                return;
            }
            auto r = pool->chatStream(ctx.request, [&](std::string_view piece) { return w.write(frame(piece, false) + "\n"); });
            if (!r.ok) {
                ++chatErrors;
                if (!w.headersSent()) {
                    const int status =
                        r.status >= net::kHttpStatusBadRequest ? r.status : net::kHttpStatusBadGateway;
                    w.setStatus(status);
                    w.setHeader("Content-Type", "application/json");
                    w.write(fail(status, "engine_error", r.error).body.dump());
                } else {
                    json::Value e = json::Value::object();
                    e["error"] = r.error;
                    w.write(e.dump() + "\n");
                }
                return;
            }
            w.write(frame("", true) + "\n");
        });
    }

    std::pair<std::optional<Params>, Result> parseJsonBody(const net::Request& req) {
        if (req.body.empty()) return {Params(json::Value::object()), Result{}};
        std::string error;
        auto parsed = json::parse(req.body, &error);
        if (!parsed) return {std::nullopt, fail(net::kHttpStatusBadRequest, "invalid_json", error)};
        if (!parsed->isObject())
            return {std::nullopt, fail(net::kHttpStatusBadRequest, "invalid_json", "request body must be a JSON object")};
        return {std::move(*parsed), Result{}};
    }

    std::string metrics() {
        std::string out;
        auto line = [&](const std::string& name, const std::string& type, const std::string& help) {
            out += std::format("# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
        };
        auto sample = [&](const std::string& name, const std::string& labels, double value) {
            out += labels.empty() ? std::format("{} {}\n", name, value) : std::format("{}{{{}}} {}\n", name, labels, value);
        };
        line("gygax_build_info", "gauge", "Build information");
        sample("gygax_build_info", std::format("version=\"{}\"", GYGAX_VERSION_STRING), 1);
        line("gygax_uptime_seconds", "gauge", "Seconds since the service started");
        sample("gygax_uptime_seconds", "",
               static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startedAt).count()));

        const auto hs = server->stats();
        line("gygax_http_requests_total", "counter", "HTTP requests parsed");
        sample("gygax_http_requests_total", "", static_cast<double>(hs.requests));
        line("gygax_http_responses_total", "counter", "HTTP responses by class");
        sample("gygax_http_responses_total", "class=\"2xx\"", static_cast<double>(hs.responses2xx));
        sample("gygax_http_responses_total", "class=\"3xx\"", static_cast<double>(hs.responses3xx));
        sample("gygax_http_responses_total", "class=\"4xx\"", static_cast<double>(hs.responses4xx));
        sample("gygax_http_responses_total", "class=\"5xx\"", static_cast<double>(hs.responses5xx));
        line("gygax_http_in_flight", "gauge", "Requests being served");
        sample("gygax_http_in_flight", "", static_cast<double>(hs.inFlight));
        line("gygax_http_rejected_overload_total", "counter", "Connections rejected because the queue was full");
        sample("gygax_http_rejected_overload_total", "", static_cast<double>(hs.rejectedOverload));
        line("gygax_http_bytes_total", "counter", "HTTP bytes by direction");
        sample("gygax_http_bytes_total", "direction=\"in\"", static_cast<double>(hs.bytesIn));
        sample("gygax_http_bytes_total", "direction=\"out\"", static_cast<double>(hs.bytesOut));

        const auto os = orchestrator->stats();
        line("gygax_agents", "gauge", "Registered agents");
        sample("gygax_agents", "", static_cast<double>(os.agents));
        line("gygax_agent_runs_total", "counter", "Agent objective runs by outcome");
        sample("gygax_agent_runs_total", "result=\"submitted\"", static_cast<double>(os.submitted));
        sample("gygax_agent_runs_total", "result=\"completed\"", static_cast<double>(os.completed));
        sample("gygax_agent_runs_total", "result=\"failed\"", static_cast<double>(os.failed));
        line("gygax_agent_model_calls_total", "counter", "Model calls issued by agents");
        sample("gygax_agent_model_calls_total", "", static_cast<double>(os.modelCalls));
        line("gygax_agent_tool_calls_total", "counter", "Tool calls issued by agents");
        sample("gygax_agent_tool_calls_total", "", static_cast<double>(os.toolCalls));
        line("gygax_agent_queue_depth", "gauge", "Objectives waiting for a worker");
        sample("gygax_agent_queue_depth", "", static_cast<double>(os.queued));

        line("gygax_chat_requests_total", "counter", "Chat completion requests");
        sample("gygax_chat_requests_total", "", static_cast<double>(chatRequests.load()));
        line("gygax_chat_errors_total", "counter", "Chat completion requests that failed");
        sample("gygax_chat_errors_total", "", static_cast<double>(chatErrors.load()));

        line("gygax_engine_healthy", "gauge", "Engine health (1 healthy)");
        line("gygax_engine_pending", "gauge", "In-flight requests per engine");
        line("gygax_engine_requests_total", "counter", "Requests per engine by result");
        line("gygax_engine_latency_ewma_ms", "gauge", "Smoothed engine latency");
        for (const auto& e : pool->status()) {
            const auto label = std::format("engine=\"{}\"", escapeLabel(e.id));
            sample("gygax_engine_healthy", label, e.healthy && e.enabled ? 1 : 0);
            sample("gygax_engine_pending", label, static_cast<double>(e.pending));
            sample("gygax_engine_requests_total", label + ",result=\"ok\"", static_cast<double>(e.completed));
            sample("gygax_engine_requests_total", label + ",result=\"error\"", static_cast<double>(e.failed));
            sample("gygax_engine_latency_ewma_ms", label, e.latencyEwmaMs);
        }
        line("gygax_devices", "gauge", "Registered devices");
        sample("gygax_devices", "", static_cast<double>(devices.ids().size()));
        line("gygax_device_connected", "gauge", "Device link state (1 connected)");
        for (const auto& id : devices.ids()) {
            if (auto d = devices.get(id)) {
                sample("gygax_device_connected", std::format("device=\"{}\",kind=\"{}\"", escapeLabel(id), d->kind()),
                       d->connected() ? 1 : 0);
            }
        }
        line("gygax_logistics_units", "gauge", "Tracked units by sku and status");
        for (const auto& g : ledger->gauges())
            sample("gygax_logistics_units", std::format("sku=\"{}\",status=\"{}\"", escapeLabel(g.sku), g.status),
                   static_cast<double>(g.count));
        line("gygax_logistics_events", "gauge", "Ledger events recorded");
        sample("gygax_logistics_events", "", static_cast<double>(ledger->eventCount()));
        return out;
    }

    void installHttp() {
        net::ServerOptions so;
        so.host = config.host;
        so.port = config.port;
        so.workers = config.httpWorkers;
        so.maxBodyBytes = config.maxBodyBytes;
        so.corsOrigin = config.corsOrigin;
        so.serverName = std::string("gygax/") + GYGAX_VERSION_STRING;
        server = std::make_unique<net::Server>(so);

        server->intercept([this](net::Request& req) -> std::optional<net::Response> {
            if (req.path == "/healthz" || req.path == "/readyz" || req.path == "/version") return std::nullopt;
            if (config.rateLimitPerMinute > 0) {
                const auto ip = stripPort(req.remote);
                const auto now = std::chrono::steady_clock::now();
                std::lock_guard lock(rateMutex);
                if (windows.size() > kMaximumRateLimitWindowEntries) {
                    std::erase_if(windows, [&](const auto& kv) { return now - kv.second.start > std::chrono::minutes(2); });
                }
                auto& w = windows[ip];
                if (now - w.start >= std::chrono::minutes(1)) {
                    w.start = now;
                    w.count = 0;
                }
                if (++w.count > config.rateLimitPerMinute) {
                    auto r = net::Response::error(net::kHttpStatusTooManyRequests, "rate_limited", "too many requests");
                    r.headers["Retry-After"] = "60";
                    return r;
                }
            }
            if (config.token.empty()) return std::nullopt;
            const auto auth = req.header("Authorization");
            constexpr std::string_view prefix = "Bearer ";
            if (auth.size() > prefix.size() && auth.compare(0, prefix.size(), prefix) == 0 &&
                net::constantTimeEquals(std::string_view(auth).substr(prefix.size()), config.token)) {
                return std::nullopt;
            }
            auto r = net::Response::error(net::kHttpStatusUnauthorized, "unauthorized", "a valid bearer token is required");
            r.headers["WWW-Authenticate"] = "Bearer";
            return r;
        });

        server->route("GET", "/healthz", [](net::Request&) {
            return net::Response::json(net::kHttpStatusOk, R"({"status":"ok"})");
        });
        server->route("GET", "/version", [](net::Request&) {
            json::Value v = json::Value::object();
            v["name"] = "gygax";
            v["version"] = GYGAX_VERSION_STRING;
            return net::Response::json(net::kHttpStatusOk, v.dump());
        });
        server->route("GET", "/readyz", [this](net::Request&) {
            const auto healthy = pool->healthyCount();
            json::Value v = json::Value::object();
            v["engines_healthy"] = healthy;
            v["engines_total"] = pool->size();
            const bool ready = running.load() && healthy > 0;
            v["status"] = ready ? "ready" : "not_ready";
            return net::Response::json(ready ? net::kHttpStatusOk : net::kHttpStatusServiceUnavailable, v.dump());
        });
        server->route("GET", "/metrics",
                      [this](net::Request&) {
                          return net::Response::text(net::kHttpStatusOk, metrics(), "text/plain; version=0.0.4; charset=utf-8");
                      });

        for (auto& op : ops) {
            if (op.rpcName == "chat.complete") continue;
            OpFn& fn = op.fn;
            server->route(op.method, op.pattern, [this, &fn](net::Request& req) {
                auto parsed = parseJsonBody(req);
                if (!parsed.first) return toResponse(parsed.second);
                Params params = std::move(*parsed.first);
                for (const auto& [k, v] : req.query) params[k] = v;
                for (const auto& [k, v] : req.params) params[k] = v;
                return toResponse(fn(params));
            });
        }

        server->route("GET", "/v1/models", [this](net::Request& r) { return listModels(r); });
        server->route("POST", "/v1/chat/completions", [this](net::Request& r) { return chatCompletions(r); });
        server->route("GET", "/api/tags", [this](net::Request& r) { return ollamaTags(r); });
        server->route("GET", "/api/version", [](net::Request&) {
            json::Value v = json::Value::object();
            v["version"] = GYGAX_VERSION_STRING;
            return net::Response::json(net::kHttpStatusOk, v.dump());
        });
        server->route("POST", "/api/chat", [this](net::Request& r) { return ollamaChat(r, false); });
        server->route("POST", "/api/generate", [this](net::Request& r) { return ollamaChat(r, true); });

        server->route("POST", "/rpc", [this](net::Request& req) {
            std::string error;
            auto parsed = json::parse(req.body, &error);
            if (!parsed) {
                json::Value e = json::Value::object();
                e["jsonrpc"] = "2.0";
                e["id"] = nullptr;
                e["error"]["code"] = -32700;
                e["error"]["message"] = "parse error: " + error;
                return net::Response::json(net::kHttpStatusOk, e.dump());
            }
            return net::Response::json(net::kHttpStatusOk, rpcDispatch(*parsed).dump());
        });

        server->route("POST", "/directive", [this](net::Request& req) {
            auto parsed = parseJsonBody(req);
            if (!parsed.first) return toResponse(parsed.second);
            const auto directive = parsed.first->getString("directive");
            if (directive.empty())
                return toResponse(fail(net::kHttpStatusBadRequest, "invalid_request", "'directive' is required"));
            auto [html, text] = brain::CoordinateWebPage();
            {
                std::lock_guard lock(layoutMutex);
                latestHtml = std::move(html);
                latestText = std::move(text);
            }
            return net::Response::json(net::kHttpStatusOk, R"({"status":"accepted"})");
        });
        server->route("GET", "/status", [this](net::Request&) {
            json::Value v = json::Value::object();
            v["status"] = "online";
            std::lock_guard lock(layoutMutex);
            v["latest_html"] = latestHtml;
            v["latest_text"] = latestText;
            return net::Response::json(net::kHttpStatusOk, v.dump());
        });
    }

    json::Value rpcSingle(const json::Value& request) {
        json::Value response = json::Value::object();
        response["jsonrpc"] = "2.0";
        const auto* id = request.find("id");
        response["id"] = id == nullptr ? json::Value(nullptr) : *id;
        auto error = [&](int code, std::string message, json::Value data = nullptr) {
            response["error"]["code"] = code;
            response["error"]["message"] = std::move(message);
            if (!data.isNull()) response["error"]["data"] = std::move(data);
            return response;
        };
        if (!request.isObject() || request.getString("jsonrpc") != "2.0" || !request.find("method") ||
            !request.find("method")->isString()) {
            return error(-32600, "invalid request");
        }
        const auto method = request.getString("method");
        auto it = rpcTable.find(method);
        if (it == rpcTable.end()) return error(-32601, "method not found: " + method);
        Params params = json::Value::object();
        if (const auto* p = request.find("params"); p != nullptr && p->isObject()) params = *p;
        Result r;
        try {
            r = it->second->fn(params);
        } catch (const std::exception& e) {
            return error(-32603, e.what());
        }
        if (r.status >= net::kHttpStatusBadRequest) {
            json::Value data = json::Value::object();
            data["status"] = r.status;
            return error(-32000, r.body.find("error") ? r.body.find("error")->getString("message") : "error", std::move(data));
        }
        response["result"] = std::move(r.body);
        return response;
    }

    json::Value rpcDispatch(const json::Value& request) {
        if (request.isArray()) {
            json::Value out = json::Value::array();
            if (request.asArray().empty()) {
                json::Value e = json::Value::object();
                e["jsonrpc"] = "2.0";
                e["id"] = nullptr;
                e["error"]["code"] = -32600;
                e["error"]["message"] = "invalid request";
                return e;
            }
            for (const auto& item : request.asArray()) out.push(rpcSingle(item));
            return out;
        }
        return rpcSingle(request);
    }
};

Service::Service(ServiceConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Service::~Service() {
    stop();
}

bool Service::start(std::string* error) {
    auto& im = *impl_;
    if (im.running.load()) return true;
    std::string verr;
    if (!im.config.validate(verr)) {
        if (error != nullptr) *error = verr;
        return false;
    }
    try {
        inference::PoolOptions po;
        po.probeInterval = std::chrono::milliseconds(im.config.probeIntervalMs);
        im.pool = std::make_shared<inference::EnginePool>(po);
        im.poolBackend = std::make_shared<inference::PoolBackend>(im.pool, im.config.defaultModel);
        inference::OrchestratorOptions oo;
        oo.workers = im.config.agentWorkers;
        oo.defaultMaxSteps = im.config.agentMaxSteps;
        oo.defaultModel = im.config.defaultModel;
        oo.protocol = im.config.toolProtocol == "atari" ? inference::ToolProtocol::Atari : inference::ToolProtocol::Json;
        im.ledger = std::make_unique<logistics::Ledger>(im.config.ledgerPath);
        im.orchestrator = std::make_unique<inference::Orchestrator>(im.poolBackend, oo);
        im.registerBuiltinTools();
        im.loadExternalModules();
        im.setupEngines();
        im.ops.clear();
        im.rpcTable.clear();
        im.defineOps();
        im.installHttp();
    } catch (const std::exception& e) {
        for (const auto& name : im.ownedTools) tools::ToolRegistry::getInstance().unregisterTool(name);
        im.ownedTools.clear();
        im.modules.clear();
        if (error != nullptr) *error = e.what();
        return false;
    }
    im.pool->probeNow();
    im.pool->startProbing();
    im.orchestrator->start();
    if (im.config.enableHttp) {
        std::string serr;
        if (!im.server->start(&serr)) {
            im.orchestrator->stop();
            im.pool->stopProbing();
            if (error != nullptr) *error = serr;
            return false;
        }
    }
    im.running.store(true);
    log::info("service", "gygax {} ready on {}:{} ({} engine(s), auth {})", GYGAX_VERSION_STRING, im.config.host,
              im.config.enableHttp ? im.server->port() : 0, im.pool->size(), im.config.token.empty() ? "disabled" : "enabled");
    return true;
}

void Service::stop() {
    auto& im = *impl_;
    if (!im.running.exchange(false)) return;
    im.orchestrator->stop();
    if (im.config.enableHttp) im.server->stop();
    im.devices.clear();
    im.pool->stopProbing();
    for (const auto& name : im.ownedTools) tools::ToolRegistry::getInstance().unregisterTool(name);
    im.modules.clear();
}

bool Service::running() const {
    return impl_->running.load();
}
std::uint16_t Service::port() const {
    return impl_->server ? impl_->server->port() : 0;
}
const ServiceConfig& Service::config() const {
    return impl_->config;
}
inference::EnginePool& Service::pool() {
    return *impl_->pool;
}
json::Value Service::rpc(const json::Value& request) {
    return impl_->rpcDispatch(request);
}
std::string Service::metricsText() {
    return impl_->metrics();
}

}
