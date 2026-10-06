#include <gygax/service/mcp.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gygax/core/json.hpp>
#include <gygax/core/log.hpp>

namespace gygax::service {

namespace {

constexpr std::size_t kMaxLine = 4 * 1024 * 1024;
constexpr std::size_t kMaxTools = 128;
constexpr int kMaximumToolListPages = 16;
constexpr std::chrono::seconds kInitializationTimeout{10};
constexpr std::chrono::seconds kToolListTimeout{10};
constexpr std::chrono::seconds kToolCallTimeout{60};
constexpr std::chrono::milliseconds kChildExitPollInterval{50};
constexpr int kChildExitPollAttempts = 20;
constexpr const char* kProtocolVersion = "2024-11-05";

std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

class McpSession {
public:
    static std::shared_ptr<McpSession> spawn(const std::vector<std::string>& argv, std::string* error) {
        int in[2];
        int out[2];
        if (::pipe(in) != 0) {
            *error = "pipe failed";
            return nullptr;
        }
        if (::pipe(out) != 0) {
            ::close(in[0]);
            ::close(in[1]);
            *error = "pipe failed";
            return nullptr;
        }
        std::vector<char*> args;
        std::vector<std::string> copy = argv;
        args.reserve(copy.size() + 1);
        for (auto& a : copy) args.push_back(a.data());
        args.push_back(nullptr);
        const pid_t pid = ::fork();
        if (pid < 0) {
            *error = "fork failed";
            return nullptr;
        }
        if (pid == 0) {
            ::dup2(in[0], 0);
            ::dup2(out[1], 1);
            const int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) ::dup2(devnull, 2);
            for (int fd : {in[0], in[1], out[0], out[1]}) ::close(fd);
            ::execvp(args[0], args.data());
            ::_exit(127);
        }
        ::close(in[0]);
        ::close(out[1]);
        ::fcntl(in[1], F_SETFD, FD_CLOEXEC);
        ::fcntl(out[0], F_SETFD, FD_CLOEXEC);
        return std::shared_ptr<McpSession>(new McpSession(pid, in[1], out[0]));
    }

    ~McpSession() {
        if (toChild_ >= 0) ::close(toChild_);
        for (int i = 0; i < kChildExitPollAttempts; ++i) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) != 0) {
                if (fromChild_ >= 0) ::close(fromChild_);
                return;
            }
            std::this_thread::sleep_for(kChildExitPollInterval);
        }
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, nullptr, 0);
        if (fromChild_ >= 0) ::close(fromChild_);
    }

    McpSession(const McpSession&) = delete;
    McpSession& operator=(const McpSession&) = delete;

    json::Value request(const std::string& method, json::Value params, std::chrono::milliseconds timeout) {
        std::lock_guard lock(mutex_);
        const std::int64_t id = ++nextId_;
        json::Value msg = json::Value::object();
        msg["jsonrpc"] = "2.0";
        msg["id"] = id;
        msg["method"] = method;
        msg["params"] = std::move(params);
        writeLine(msg.dump());
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const std::string line = readLine(deadline);
            auto parsed = json::parse(line);
            if (!parsed || !parsed->isObject()) continue;
            const auto* rid = parsed->find("id");
            if (rid == nullptr || !rid->isInt() || rid->asInt() != id || parsed->find("method") != nullptr) continue;
            if (const auto* err = parsed->find("error")) {
                std::string text = err->isObject() ? err->getString("message") : std::string();
                throw std::runtime_error("mcp error: " + (text.empty() ? err->dump() : text));
            }
            const auto* result = parsed->find("result");
            return result != nullptr ? *result : json::Value::object();
        }
    }

    void notify(const std::string& method) {
        std::lock_guard lock(mutex_);
        json::Value msg = json::Value::object();
        msg["jsonrpc"] = "2.0";
        msg["method"] = method;
        writeLine(msg.dump());
    }

private:
    McpSession(pid_t pid, int toChild, int fromChild) : pid_(pid), toChild_(toChild), fromChild_(fromChild) {}

    void writeLine(const std::string& text) {
        const std::string line = text + "\n";
        std::size_t done = 0;
        while (done < line.size()) {
            const ssize_t n = ::write(toChild_, line.data() + done, line.size() - done);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("mcp server closed its input");
            }
            done += static_cast<std::size_t>(n);
        }
    }

    std::string readLine(std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            const auto nl = buffer_.find('\n');
            if (nl != std::string::npos) {
                std::string line = buffer_.substr(0, nl);
                buffer_.erase(0, nl + 1);
                return line;
            }
            if (buffer_.size() > kMaxLine) throw std::runtime_error("mcp message too large");
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) throw std::runtime_error("mcp request timed out");
            pollfd p{fromChild_, POLLIN, 0};
            const int r = ::poll(&p, 1, static_cast<int>(left.count()));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) throw std::runtime_error("mcp request timed out");
            char chunk[4096];
            const ssize_t n = ::read(fromChild_, chunk, sizeof(chunk));
            if (n <= 0) throw std::runtime_error("mcp server exited");
            buffer_.append(chunk, static_cast<std::size_t>(n));
        }
    }

    pid_t pid_;
    int toChild_;
    int fromChild_;
    std::mutex mutex_;
    std::string buffer_;
    std::int64_t nextId_ = 0;
};

std::string sanitizeToolName(const std::string& name) {
    std::string out;
    for (char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
    return out;
}

std::string renderContent(const json::Value& result) {
    std::string text;
    if (const auto* content = result.find("content"); content != nullptr && content->isArray()) {
        for (const auto& part : content->asArray()) {
            if (!text.empty()) text += "\n";
            const auto type = part.getString("type");
            if (type == "text")
                text += part.getString("text");
            else
                text += "[" + (type.empty() ? std::string("unknown") : type) + " content omitted]";
        }
    }
    if (text.empty()) {
        if (const auto* structured = result.find("structuredContent")) text = structured->dump();
    }
    return text;
}

}

std::optional<ExternalModule> connectMcp(const std::string& spec, std::string* error) {
    auto fail = [&](std::string m) -> std::optional<ExternalModule> {
        if (error != nullptr) *error = "mcp " + spec + ": " + std::move(m);
        return std::nullopt;
    };
    const auto eq = spec.find('=');
    if (eq == std::string::npos) return fail("expected NAME=COMMAND [ARGS...]");
    ExternalModule m;
    m.kind = "mcp";
    m.name = spec.substr(0, eq);
    m.source = spec.substr(eq + 1);
    if (!validPluginName(m.name)) return fail("name must be 1-32 characters of [a-z0-9_-]");
    const auto argv = splitWords(m.source);
    if (argv.empty()) return fail("missing command");

    std::string spawnError;
    auto session = McpSession::spawn(argv, &spawnError);
    if (!session) return fail(spawnError);
    try {
        json::Value params = json::Value::object();
        params["protocolVersion"] = kProtocolVersion;
        params["capabilities"] = json::Value::object();
        json::Value client = json::Value::object();
        client["name"] = "gygax";
        client["version"] = "0.2";
        params["clientInfo"] = std::move(client);
        const auto init = session->request("initialize", std::move(params), kInitializationTimeout);
        if (const auto* info = init.find("serverInfo")) m.version = info->getString("version");
        session->notify("notifications/initialized");

        std::string cursor;
        for (int page = 0; page < kMaximumToolListPages; ++page) {
            json::Value listParams = json::Value::object();
            if (!cursor.empty()) listParams["cursor"] = cursor;
            const auto listed = session->request("tools/list", std::move(listParams), kToolListTimeout);
            const auto* tools = listed.find("tools");
            if (tools == nullptr || !tools->isArray()) return fail("tools/list did not return a tool array");
            for (const auto& t : tools->asArray()) {
                const std::string remote = t.getString("name");
                const std::string local = sanitizeToolName(remote);
                if (!validPluginToolName(local)) {
                    log::warn("mcp", "skipping tool with unusable name '{}'", remote);
                    continue;
                }
                if (m.tools.size() >= kMaxTools) return fail("server lists more than 128 tools");
                ExternalTool tool;
                tool.name = local;
                tool.description = t.getString("description");
                tool.invoke = [session, remote](const std::string& input) {
                    json::Value args = json::Value::object();
                    if (auto parsed = json::parse(input); parsed && parsed->isObject())
                        args = std::move(*parsed);
                    else if (!input.empty())
                        args["input"] = input;
                    json::Value call = json::Value::object();
                    call["name"] = remote;
                    call["arguments"] = std::move(args);
                    const auto result = session->request("tools/call", std::move(call), kToolCallTimeout);
                    std::string text = renderContent(result);
                    if (text.size() > kMaxLine) throw std::runtime_error("mcp output too large");
                    if (result.getBool("isError")) throw std::runtime_error(text.empty() ? "mcp tool failed" : text);
                    return text;
                };
                m.tools.push_back(std::move(tool));
            }
            cursor = listed.getString("nextCursor");
            if (cursor.empty()) break;
        }
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    m.keepAlive = session;
    return m;
}

int serveMcp(int inFd, int outFd, const std::string& serverName, const std::string& version, const McpServerHandler& handler) {
    auto send = [&](const json::Value& v) {
        const std::string line = v.dump() + "\n";
        std::size_t done = 0;
        while (done < line.size()) {
            const ssize_t n = ::write(outFd, line.data() + done, line.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            done += static_cast<std::size_t>(n);
        }
        return true;
    };
    auto reply = [&](const json::Value& id, json::Value result) {
        json::Value m = json::Value::object();
        m["jsonrpc"] = "2.0";
        m["id"] = id;
        m["result"] = std::move(result);
        return send(m);
    };
    auto fault = [&](const json::Value& id, int code, const std::string& text) {
        json::Value m = json::Value::object();
        m["jsonrpc"] = "2.0";
        m["id"] = id;
        json::Value e = json::Value::object();
        e["code"] = code;
        e["message"] = text;
        m["error"] = std::move(e);
        return send(m);
    };

    std::string buffer;
    char chunk[4096];
    for (;;) {
        const auto nl = buffer.find('\n');
        if (nl == std::string::npos) {
            if (buffer.size() > kMaxLine) return 1;
            const ssize_t n = ::read(inFd, chunk, sizeof(chunk));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return 0;
            buffer.append(chunk, static_cast<std::size_t>(n));
            continue;
        }
        const std::string line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        auto parsed = json::parse(line);
        if (!parsed || !parsed->isObject()) {
            if (!fault(json::Value(), -32700, "parse error")) return 1;
            continue;
        }
        const auto* idp = parsed->find("id");
        if (idp == nullptr) continue;
        const json::Value id = *idp;
        const std::string method = parsed->getString("method");
        const auto* params = parsed->find("params");
        bool ok = true;
        if (method == "initialize") {
            json::Value r = json::Value::object();
            const std::string requested = params != nullptr ? params->getString("protocolVersion") : std::string();
            r["protocolVersion"] = requested.empty() ? std::string(kProtocolVersion) : requested;
            json::Value caps = json::Value::object();
            caps["tools"] = json::Value::object();
            r["capabilities"] = std::move(caps);
            json::Value info = json::Value::object();
            info["name"] = serverName;
            info["version"] = version;
            r["serverInfo"] = std::move(info);
            ok = reply(id, std::move(r));
        } else if (method == "ping") {
            ok = reply(id, json::Value::object());
        } else if (method == "tools/list") {
            json::Value tools = json::Value::array();
            for (const auto& t : handler.list()) {
                json::Value o = json::Value::object();
                o["name"] = t.name;
                o["description"] = t.description;
                json::Value schema = json::Value::object();
                schema["type"] = "object";
                o["inputSchema"] = std::move(schema);
                tools.push(std::move(o));
            }
            json::Value r = json::Value::object();
            r["tools"] = std::move(tools);
            ok = reply(id, std::move(r));
        } else if (method == "tools/call") {
            if (params == nullptr || !params->isObject() || params->getString("name").empty()) {
                ok = fault(id, -32602, "tools/call needs a tool name");
            } else {
                json::Value args = json::Value::object();
                if (const auto* a = params->find("arguments"); a != nullptr && a->isObject()) args = *a;
                json::Value r = json::Value::object();
                bool isError = false;
                std::string text;
                try {
                    text = handler.call(params->getString("name"), args.dump());
                } catch (const std::exception& e) {
                    isError = true;
                    text = e.what();
                }
                json::Value part = json::Value::object();
                part["type"] = "text";
                part["text"] = text;
                json::Value content = json::Value::array();
                content.push(std::move(part));
                r["content"] = std::move(content);
                r["isError"] = isError;
                ok = reply(id, std::move(r));
            }
        } else {
            ok = fault(id, -32601, "method not found: " + method);
        }
        if (!ok) return 1;
    }
}

}
