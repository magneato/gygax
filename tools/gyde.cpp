#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <gygax/core/json.hpp>
#include <gygax/net/http.hpp>
#include <gygax/version.hpp>

namespace {

using gygax::json::Value;
namespace net = gygax::net;

constexpr char kDefaultServiceUrl[] = "http://127.0.0.1:1984";
constexpr int kHttpSuccessStatus = 200;
constexpr auto kConnectTimeout = std::chrono::milliseconds(1500);
constexpr auto kReadTimeout = std::chrono::seconds(120);

// gyde ("guide") is the terminal preview of the Gygax IDE described in
// docs/GYDE.md: a console that always shows the service's current status and
// lets you type commands against it. It is Phase 1 of that design: a real,
// working piece of it, not a mock of the eventual desktop app. The panels,
// spinning logo, training and broadcast views, and Python extensibility are
// future phases and are not implemented here; see docs/GYDE.md for scope.

struct Remote {
    std::string base;
    std::string token;

    [[nodiscard]] net::ClientResponse call(const std::string& method, const std::string& path, const std::string& body = {}) const {
        std::string error;
        auto url = net::Url::parse(base + path, &error);
        if (!url) {
            net::ClientResponse r;
            r.error = error;
            return r;
        }
        net::Headers h;
        h["Content-Type"] = "application/json";
        if (!token.empty()) h["Authorization"] = "Bearer " + token;
        net::ClientOptions o;
        o.connectTimeout = kConnectTimeout;
        o.readTimeout = kReadTimeout;
        return net::httpRequest(*url, method, body, h, o);
    }
};

struct StatusFlash {
    bool reachable = false;
    bool healthy = false;
    std::uint64_t enginesHealthy = 0;
    std::uint64_t enginesTotal = 0;
    std::uint64_t agents = 0;
    std::string error;
};

StatusFlash fetchStatus(const Remote& remote) {
    StatusFlash s;
    const auto health = remote.call("GET", "/healthz");
    if (!health.error.empty()) {
        s.error = health.error;
        return s;
    }
    s.reachable = true;
    s.healthy = health.status == kHttpSuccessStatus;
    if (const auto engines = remote.call("GET", "/v1/engines"); engines.status == kHttpSuccessStatus) {
        if (const auto parsed = gygax::json::parse(engines.body)) {
            if (const auto* list = parsed->find("engines")) {
                for (const auto& e : list->asArray()) {
                    ++s.enginesTotal;
                    if (e.getBool("healthy")) ++s.enginesHealthy;
                }
            }
        }
    }
    if (const auto agents = remote.call("GET", "/v1/agents"); agents.status == kHttpSuccessStatus) {
        if (const auto parsed = gygax::json::parse(agents.body)) {
            if (const auto* list = parsed->find("agents")) s.agents = list->asArray().size();
        }
    }
    return s;
}

std::string renderFlash(const StatusFlash& s, const std::string& base) {
    std::ostringstream out;
    if (!s.reachable) {
        out << "[unreachable: " << s.error << "] " << base;
        return out.str();
    }
    out << "[" << (s.healthy ? "up" : "DOWN") << "] engines " << s.enginesHealthy << "/" << s.enginesTotal << "  agents " << s.agents
        << "  " << base;
    return out.str();
}

std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

void printHelp() {
    std::cout << "gyde commands:\n"
                 "  status                 node, engines and agents (full detail)\n"
                 "  engines                engine health table\n"
                 "  models                 models available for chat\n"
                 "  ask <text>             send a chat completion (default model, or --model M at startup)\n"
                 "  tool <name> <input>    invoke a registered tool, e.g. tool math.eval 6*7\n"
                 "  refresh                re-fetch the status flash shown at every prompt\n"
                 "  help                   this text\n"
                 "  quit | exit\n";
}

int cmdStatus(const Remote& remote) {
    for (const char* path : {"/v1/node", "/v1/engines", "/v1/agents"}) {
        const auto r = remote.call("GET", path);
        if (!r.error.empty() || r.status != 200) {
            std::cout << path << ": unreachable (" << (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error) << ")\n";
            continue;
        }
        const auto parsed = gygax::json::parse(r.body);
        std::cout << "== " << path << " ==\n" << (parsed ? parsed->dump(2) : r.body) << "\n";
    }
    return 0;
}

int cmdEngines(const Remote& remote) {
    const auto r = remote.call("GET", "/v1/engines");
    if (r.status != kHttpSuccessStatus) {
        std::cout << "engines: HTTP " << r.status << "\n";
        return 1;
    }
    const auto parsed = gygax::json::parse(r.body);
    if (!parsed) return 1;
    for (const auto& e : parsed->find("engines")->asArray()) {
        std::cout << (e.getBool("healthy") ? "  up  " : "  DOWN ") << e.getString("id") << "  " << e.getString("kind")
                  << "  pending=" << e.getInt("pending") << "  latency_ms=" << static_cast<long long>(e.getDouble("latency_ewma_ms"))
                  << "\n";
    }
    return 0;
}

int cmdAsk(const Remote& remote, const std::string& model, const std::string& text) {
    if (text.empty()) {
        std::cout << "usage: ask <text>\n";
        return 2;
    }
    std::string useModel = model;
    if (useModel.empty()) {
        const auto models = remote.call("GET", "/v1/models");
        if (const auto parsed = gygax::json::parse(models.body))
            if (const auto* data = parsed->find("data"); data != nullptr && !data->asArray().empty())
                useModel = data->asArray().front().getString("id");
    }
    Value body = Value::object();
    body["model"] = useModel;
    Value msg = Value::object();
    msg["role"] = "user";
    msg["content"] = text;
    body["messages"].push(std::move(msg));
    const auto r = remote.call("POST", "/v1/chat/completions", body.dump());
    if (!r.error.empty()) {
        std::cout << "ask: " << r.error << "\n";
        return 1;
    }
    const auto parsed = gygax::json::parse(r.body);
    if (r.status != kHttpSuccessStatus || !parsed) {
        std::cout << "ask: HTTP " << r.status << ": " << r.body << "\n";
        return 1;
    }
    const auto* choices = parsed->find("choices");
    std::cout << (choices && !choices->asArray().empty() ? choices->asArray().front().find("message")->getString("content") : "") << "\n";
    return 0;
}

int cmdTool(const Remote& remote, const std::string& name, const std::string& input) {
    if (name.empty()) {
        std::cout << "usage: tool <name> <input>\n";
        return 2;
    }
    Value body = Value::object();
    body["input"] = input;
    const auto r = remote.call("POST", "/v1/tools/" + name + "/invoke", body.dump());
    if (!r.error.empty()) {
        std::cout << "tool: " << r.error << "\n";
        return 1;
    }
    std::cout << r.body << "\n";
    return 0;
}

}

int main(int argc, char** argv) {
    Remote remote;
    remote.base = kDefaultServiceUrl;
    std::string defaultModel;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (arg == "--url")
            remote.base = next();
        else if (arg == "--token")
            remote.token = next();
        else if (arg == "--model")
            defaultModel = next();
        else if (arg == "--help" || arg == "-h") {
            std::cout << "gyde " << GYGAX_VERSION_STRING
                      << " - terminal preview of the Gygax IDE (docs/GYDE.md)\n"
                         "usage: gyde [--url URL] [--token TOKEN] [--model MODEL]\n";
            return 0;
        } else {
            std::cerr << "gyde: unknown option '" << arg << "'\n";
            return 2;
        }
    }
    while (!remote.base.empty() && remote.base.back() == '/') remote.base.pop_back();
    if (remote.token.empty()) {
        if (const char* env = std::getenv("GYGAX_API_TOKEN")) remote.token = env;
    }

    std::cout << "gyde " << GYGAX_VERSION_STRING << " - type 'help' for commands, 'quit' to leave\n";
    auto flash = fetchStatus(remote);
    std::cout << renderFlash(flash, remote.base) << "\n";

    std::string line;
    for (;;) {
        std::cout << renderFlash(flash, remote.base) << " > " << std::flush;
        if (!std::getline(std::cin, line)) break;
        const auto words = splitWords(line);
        if (words.empty()) continue;
        const auto& cmd = words[0];
        const auto rest = line.substr(line.find(cmd) + cmd.size());
        auto trim = [](const std::string& s) {
            const auto a = s.find_first_not_of(" \t");
            if (a == std::string::npos) return std::string();
            return s.substr(a);
        };
        if (cmd == "quit" || cmd == "exit") {
            break;
        } else if (cmd == "help") {
            printHelp();
        } else if (cmd == "status") {
            cmdStatus(remote);
        } else if (cmd == "engines") {
            cmdEngines(remote);
        } else if (cmd == "models") {
            const auto r = remote.call("GET", "/v1/models");
            std::cout << r.body << "\n";
        } else if (cmd == "ask") {
            cmdAsk(remote, defaultModel, trim(rest));
        } else if (cmd == "tool") {
            const auto toolRest = trim(rest);
            const auto sp = toolRest.find(' ');
            const auto name = sp == std::string::npos ? toolRest : toolRest.substr(0, sp);
            const auto input = sp == std::string::npos ? std::string() : trim(toolRest.substr(sp + 1));
            cmdTool(remote, name, input);
        } else if (cmd == "refresh") {
            // handled below
        } else {
            std::cout << "unknown command '" << cmd << "'; try 'help'\n";
        }
        flash = fetchStatus(remote);
    }
    std::cout << "bye\n";
    return 0;
}
