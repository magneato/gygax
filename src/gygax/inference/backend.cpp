#include <gygax/inference/backend.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>

#include <gygax/core/log.hpp>
#include <gygax/inference/atari.hpp>

namespace gygax::inference {

namespace {

std::string lowerCopy(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return out;
}

int estimateTokens(std::string_view text) {
    int words = 0;
    bool inWord = false;
    for (const char c : text) {
        const bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (!space && !inWord) ++words;
        inWord = !space;
    }
    return words;
}

bool retryableStatus(int status) {
    return status == 0 || status == 408 || status == 425 || status == 429 || status >= 500;
}

std::string extractErrorMessage(const std::string& body) {
    if (auto parsed = json::parse(body)) {
        if (const auto* err = parsed->find("error")) {
            if (err->isString()) return err->asString();
            if (err->isObject()) return err->getString("message");
        }
    }
    return body.substr(0, 200);
}

std::int64_t elapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

}

std::string normalizeModel(std::string_view name) {
    std::string out = lowerCopy(name);
    constexpr std::string_view suffix = ":latest";
    if (out.size() > suffix.size() && out.ends_with(suffix)) out.resize(out.size() - suffix.size());
    return out;
}

ChatResult Backend::chatStream(const ChatRequest& request, const DeltaCallback& onDelta) {
    ChatResult result = chat(request);
    if (result.ok && onDelta) onDelta(result.text);
    return result;
}

ChatResult EchoBackend::chat(const ChatRequest& request) {
    const auto start = std::chrono::steady_clock::now();
    ChatResult result;
    result.model = request.model.empty() ? "echo" : request.model;
    result.engine = "echo";

    bool protocol = false;
    std::string lastUser;
    std::string systemMessage;
    for (const auto& m : request.messages) {
        if (m.role == "system" && m.content.find(kProtocolMarker) != std::string::npos) {
            protocol = true;
            systemMessage = m.content;
        }
        if (m.role == "user") lastUser = m.content;
    }
    if (request.messages.empty()) {
        result.status = 400;
        result.error = "messages must not be empty";
        return result;
    }

    constexpr std::string_view toolCommand = "!tool ";
    const auto schemaAt = systemMessage.find("ATARI::SCHEMA(");
    if (!protocol) {
        result.text = "echo: " + lastUser;
    } else if (schemaAt != std::string::npos) {
        // The system prompt asked for the ATARI grammar (gygax/inference/atari.hpp); reply in kind
        // instead of the JSON tool protocol below, or an ATARI::RESULT/ERROR frame would come back
        // as a garbled literal "answer".
        if (const auto toolReply = atari::parse(lastUser); toolReply.type == atari::FrameType::Result) {
            result.text = atari::encodeDone("echo: " + toolReply.get("o"));
        } else if (toolReply.type == atari::FrameType::Error) {
            result.text = atari::encodeDone("echo: " + toolReply.get("e"));
        } else if (lastUser.starts_with(toolCommand)) {
            const auto rest = lastUser.substr(toolCommand.size());
            const auto space = rest.find(' ');
            const auto toolName = rest.substr(0, space);
            const auto input = space == std::string::npos ? std::string() : rest.substr(space + 1);
            const auto schema = atari::parse(systemMessage.substr(schemaAt));
            std::string code = toolName;
            for (const auto& [c, name] : schema.fields)
                if (name == toolName) code = c;
            result.text = atari::encodeCall(code, input);
        } else {
            result.text = atari::encodeDone("echo: " + lastUser);
        }
    } else {
        json::Value reply = json::Value::object();
        constexpr std::string_view toolResult = "Tool result (";
        if (lastUser.starts_with(toolResult)) {
            const auto close = lastUser.find("): ");
            reply["answer"] = close == std::string::npos ? lastUser : lastUser.substr(close + 3);
        } else if (lastUser.starts_with(toolCommand)) {
            const auto rest = lastUser.substr(toolCommand.size());
            const auto space = rest.find(' ');
            reply["tool"] = rest.substr(0, space);
            reply["input"] = space == std::string::npos ? std::string() : rest.substr(space + 1);
        } else {
            reply["answer"] = "echo: " + lastUser;
        }
        result.text = reply.dump();
    }
    result.ok = true;
    result.status = 200;
    for (const auto& m : request.messages) result.promptTokens += estimateTokens(m.content);
    result.completionTokens = estimateTokens(result.text);
    result.latencyMs = elapsedMs(start);
    return result;
}

bool EchoBackend::listModels(std::vector<std::string>& out, std::string&) {
    out = {"echo"};
    return true;
}

ChatApiBackend::ChatApiBackend(ChatApiOptions options) : options_(std::move(options)) {
    while (options_.baseUrl.size() > 1 && options_.baseUrl.back() == '/') options_.baseUrl.pop_back();
}

std::optional<net::Url> ChatApiBackend::url(std::string_view suffix, std::string& error) const {
    auto parsed = net::Url::parse(options_.baseUrl + std::string(suffix), &error);
    return parsed;
}

net::Headers ChatApiBackend::headers() const {
    net::Headers h;
    h["Content-Type"] = "application/json";
    h["Accept"] = "application/json";
    if (!options_.apiKey.empty()) h["Authorization"] = "Bearer " + options_.apiKey;
    for (const auto& [k, v] : options_.extraHeaders) h[k] = v;
    return h;
}

json::Value ChatApiBackend::buildBody(const ChatRequest& request, bool stream) const {
    json::Value body = json::Value::object();
    body["model"] = request.model.empty() ? options_.defaultModel : request.model;
    json::Value messages = json::Value::array();
    for (const auto& m : request.messages) {
        json::Value item = json::Value::object();
        item["role"] = m.role;
        item["content"] = m.content;
        messages.push(std::move(item));
    }
    body["messages"] = std::move(messages);
    body["temperature"] = request.temperature;
    if (request.maxTokens > 0) body["max_tokens"] = request.maxTokens;
    body["stream"] = stream;
    return body;
}

ChatResult ChatApiBackend::chat(const ChatRequest& request) {
    const auto start = std::chrono::steady_clock::now();
    ChatResult result;
    result.engine = options_.baseUrl;
    std::string error;
    auto target = url("/chat/completions", error);
    if (!target) {
        result.error = error;
        result.status = 400;
        return result;
    }
    net::ClientOptions opts;
    opts.connectTimeout = options_.connectTimeout;
    opts.readTimeout = options_.readTimeout;
    auto response = net::httpRequest(*target, "POST", buildBody(request, false).dump(), headers(), opts);
    result.latencyMs = elapsedMs(start);
    result.status = response.status;
    if (!response.error.empty()) {
        result.error = response.error;
        result.retryable = true;
        return result;
    }
    if (response.status < 200 || response.status >= 300) {
        result.error = extractErrorMessage(response.body);
        result.retryable = retryableStatus(response.status) || response.status == 404;
        return result;
    }
    auto parsed = json::parse(response.body);
    if (!parsed) {
        result.error = "engine returned invalid JSON";
        result.retryable = true;
        return result;
    }
    const auto& choices = parsed->find("choices");
    if (choices == nullptr || choices->asArray().empty()) {
        result.error = "engine response has no choices";
        result.retryable = true;
        return result;
    }
    const auto* message = choices->asArray().front().find("message");
    result.text = message != nullptr ? message->getString("content") : std::string();
    result.model = parsed->getString("model", request.model);
    if (const auto* usage = parsed->find("usage")) {
        result.promptTokens = static_cast<int>(usage->getInt("prompt_tokens"));
        result.completionTokens = static_cast<int>(usage->getInt("completion_tokens"));
    }
    result.ok = true;
    return result;
}

ChatResult ChatApiBackend::chatStream(const ChatRequest& request, const DeltaCallback& onDelta) {
    const auto start = std::chrono::steady_clock::now();
    ChatResult result;
    result.engine = options_.baseUrl;
    std::string error;
    auto target = url("/chat/completions", error);
    if (!target) {
        result.error = error;
        result.status = 400;
        return result;
    }
    net::ClientOptions opts;
    opts.connectTimeout = options_.connectTimeout;
    opts.readTimeout = options_.readTimeout;

    std::string pending;
    std::string errorBody;
    bool failedStatus = false;
    bool done = false;
    bool stopped = false;

    auto handleLine = [&](std::string_view line) {
        if (!line.starts_with("data:")) return;
        line.remove_prefix(5);
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        if (line == "[DONE]") {
            done = true;
            return;
        }
        auto parsed = json::parse(line);
        if (!parsed) return;
        if (result.model.empty()) result.model = parsed->getString("model");
        const auto* choices = parsed->find("choices");
        if (choices == nullptr || choices->asArray().empty()) return;
        const auto* delta = choices->asArray().front().find("delta");
        if (delta == nullptr) return;
        const auto& piece = delta->getString("content");
        if (piece.empty()) return;
        result.text += piece;
        if (onDelta && !onDelta(piece)) stopped = true;
    };

    auto response = net::httpStream(
        *target, "POST", buildBody(request, true).dump(), headers(), opts,
        [&](int status, const net::Headers&) {
            result.status = status;
            failedStatus = status < 200 || status >= 300;
            return true;
        },
        [&](std::string_view data) {
            if (failedStatus) {
                errorBody.append(data);
                return true;
            }
            pending.append(data);
            std::size_t pos = 0;
            while (!stopped) {
                const auto nl = pending.find('\n', pos);
                if (nl == std::string::npos) break;
                std::string_view line = std::string_view(pending).substr(pos, nl - pos);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                handleLine(line);
                pos = nl + 1;
            }
            pending.erase(0, pos);
            return !stopped && !done;
        });
    result.latencyMs = elapsedMs(start);
    if (result.status == 0) result.status = response.status;
    if (!response.error.empty()) {
        result.error = response.error;
        result.retryable = result.text.empty();
        return result;
    }
    if (failedStatus) {
        result.error = extractErrorMessage(errorBody);
        result.retryable = retryableStatus(result.status) || result.status == 404;
        return result;
    }
    result.ok = true;
    result.completionTokens = estimateTokens(result.text);
    return result;
}

bool ChatApiBackend::listModels(std::vector<std::string>& out, std::string& error) {
    auto target = url("/models", error);
    if (!target) return false;
    net::ClientOptions opts;
    opts.connectTimeout = options_.connectTimeout;
    opts.readTimeout = std::chrono::milliseconds(5000);
    auto response = net::httpRequest(*target, "GET", {}, headers(), opts);
    if (!response.error.empty()) {
        error = response.error;
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        error = std::format("engine answered HTTP {}", response.status);
        return false;
    }
    auto parsed = json::parse(response.body);
    if (!parsed) {
        error = "engine returned invalid JSON";
        return false;
    }
    out.clear();
    for (const auto& item : parsed->find("data") != nullptr ? parsed->find("data")->asArray() : json::Array{}) {
        const auto id = item.getString("id");
        if (!id.empty()) out.push_back(id);
    }
    return true;
}

EnginePool::EnginePool(PoolOptions options) : options_(options) {}

EnginePool::~EnginePool() {
    stopProbing();
}

bool EnginePool::addEngine(std::string id, std::shared_ptr<Backend> backend, std::string* error) {
    if (id.empty() || !backend) {
        if (error != nullptr) *error = "engine id and backend are required";
        return false;
    }
    std::lock_guard lock(mutex_);
    for (const auto& e : engines_) {
        if (e->id == id) {
            if (error != nullptr) *error = "engine id already registered: " + id;
            return false;
        }
    }
    auto engine = std::make_unique<Engine>();
    engine->id = std::move(id);
    engine->backend = std::move(backend);
    engines_.push_back(std::move(engine));
    return true;
}

bool EnginePool::removeEngine(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto before = engines_.size();
    std::erase_if(engines_, [&](const auto& e) { return e->id == id; });
    if (pinned_ && *pinned_ == id) pinned_.reset();
    return engines_.size() != before;
}

bool EnginePool::setEnabled(const std::string& id, bool enabled) {
    std::lock_guard lock(mutex_);
    for (auto& e : engines_) {
        if (e->id == id) {
            e->enabled = enabled;
            return true;
        }
    }
    return false;
}

void EnginePool::pin(std::optional<std::string> id) {
    std::lock_guard lock(mutex_);
    pinned_ = std::move(id);
}

std::optional<std::string> EnginePool::pinned() const {
    std::lock_guard lock(mutex_);
    return pinned_;
}

std::size_t EnginePool::size() const {
    std::lock_guard lock(mutex_);
    return engines_.size();
}

std::size_t EnginePool::healthyCount() const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::ranges::count_if(engines_, [](const auto& e) { return e->healthy && e->enabled; }));
}

std::vector<std::string> EnginePool::rank(const ChatRequest& request) const {
    std::lock_guard lock(mutex_);
    const auto wanted = normalizeModel(request.model);
    struct Candidate {
        std::string id;
        double score;
        bool pinned;
    };
    std::vector<Candidate> candidates;
    for (const auto& e : engines_) {
        if (!e->enabled || !e->healthy) continue;
        if (request.localOnly && e->backend->isRemote()) continue;
        const bool eligible = wanted.empty() || e->backend->acceptsAnyModel() || !e->inventoryKnown || e->models.contains(wanted);
        if (!eligible) continue;
        candidates.push_back({e->id, static_cast<double>(e->pending) * 1000.0 + e->latencyEwmaMs, pinned_ && *pinned_ == e->id});
    }
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        if (a.pinned != b.pinned) return a.pinned;
        if (a.score != b.score) return a.score < b.score;
        return a.id < b.id;
    });
    std::vector<std::string> ids;
    ids.reserve(candidates.size());
    for (auto& c : candidates) ids.push_back(std::move(c.id));
    return ids;
}

void EnginePool::record(const std::string& id, const ChatResult& result) {
    std::lock_guard lock(mutex_);
    for (auto& e : engines_) {
        if (e->id != id) continue;
        if (e->pending > 0) --e->pending;
        if (result.ok) {
            ++e->completed;
            e->consecutiveFailures = 0;
            e->healthy = true;
            e->lastError.clear();
            const double sample = static_cast<double>(result.latencyMs);
            e->latencyEwmaMs =
                e->latencyEwmaMs == 0.0 ? sample : options_.ewmaAlpha * sample + (1.0 - options_.ewmaAlpha) * e->latencyEwmaMs;
        } else {
            ++e->failed;
            e->lastError = result.error;
            if (result.retryable && result.status != 404) {
                if (++e->consecutiveFailures >= options_.failuresBeforeUnhealthy) e->healthy = false;
            }
        }
        return;
    }
}

ChatResult EnginePool::chat(const ChatRequest& request) {
    const auto candidates = rank(request);
    if (candidates.empty()) {
        ChatResult none;
        none.status = 503;
        none.error =
            request.model.empty() ? "no healthy inference engine available" : "no healthy engine advertises model '" + request.model + "'";
        return none;
    }
    ChatResult last;
    for (const auto& id : candidates) {
        std::shared_ptr<Backend> backend;
        {
            std::lock_guard lock(mutex_);
            for (auto& e : engines_) {
                if (e->id == id) {
                    backend = e->backend;
                    ++e->pending;
                }
            }
        }
        if (!backend) continue;
        ChatResult result = backend->chat(request);
        result.engine = id;
        record(id, result);
        if (result.ok || !result.retryable) return result;
        log::warn("pool", "engine {} failed ({}); trying next candidate", id, result.error);
        last = std::move(result);
    }
    if (last.status < 400) last.status = 502;
    return last;
}

ChatResult EnginePool::chatStream(const ChatRequest& request, const DeltaCallback& onDelta) {
    const auto candidates = rank(request);
    if (candidates.empty()) {
        ChatResult none;
        none.status = 503;
        none.error =
            request.model.empty() ? "no healthy inference engine available" : "no healthy engine advertises model '" + request.model + "'";
        return none;
    }
    ChatResult last;
    for (const auto& id : candidates) {
        std::shared_ptr<Backend> backend;
        {
            std::lock_guard lock(mutex_);
            for (auto& e : engines_) {
                if (e->id == id) {
                    backend = e->backend;
                    ++e->pending;
                }
            }
        }
        if (!backend) continue;
        bool delivered = false;
        ChatResult result = backend->chatStream(request, [&](std::string_view delta) {
            delivered = true;
            return onDelta ? onDelta(delta) : true;
        });
        result.engine = id;
        if (delivered && !result.ok) result.retryable = false;
        record(id, result);
        if (result.ok || !result.retryable) return result;
        log::warn("pool", "engine {} failed ({}); trying next candidate", id, result.error);
        last = std::move(result);
    }
    if (last.status < 400) last.status = 502;
    return last;
}

void EnginePool::probeEngine(const std::string& id) {
    std::shared_ptr<Backend> backend;
    {
        std::lock_guard lock(mutex_);
        for (auto& e : engines_) {
            if (e->id == id) backend = e->backend;
        }
    }
    if (!backend) return;
    std::vector<std::string> names;
    std::string error;
    const bool ok = backend->listModels(names, error);
    std::lock_guard lock(mutex_);
    for (auto& e : engines_) {
        if (e->id != id) continue;
        e->lastProbe = std::chrono::steady_clock::now();
        if (ok) {
            e->healthy = true;
            e->consecutiveFailures = 0;
            e->inventoryKnown = true;
            e->models.clear();
            for (const auto& n : names) e->models.insert(normalizeModel(n));
            e->lastError.clear();
        } else {
            e->healthy = false;
            e->lastError = error;
        }
        return;
    }
}

void EnginePool::probeNow() {
    std::vector<std::string> ids;
    {
        std::lock_guard lock(mutex_);
        for (const auto& e : engines_) ids.push_back(e->id);
    }
    for (const auto& id : ids) probeEngine(id);
}

void EnginePool::probeLoop(const std::stop_token& stop) {
    while (!stop.stop_requested()) {
        std::vector<std::string> due;
        {
            std::lock_guard lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            for (const auto& e : engines_) {
                const auto interval = e->healthy ? options_.probeInterval : options_.unhealthyProbeInterval;
                if (e->lastProbe == std::chrono::steady_clock::time_point{} || now - e->lastProbe >= interval) due.push_back(e->id);
            }
        }
        for (const auto& id : due) {
            if (stop.stop_requested()) return;
            probeEngine(id);
        }
        std::unique_lock lock(probeMutex_);
        probeWake_.wait_for(lock, stop, std::chrono::milliseconds(500), [] { return false; });
    }
}

void EnginePool::startProbing() {
    if (prober_.joinable()) return;
    prober_ = std::jthread([this](const std::stop_token& st) { probeLoop(st); });
}

void EnginePool::stopProbing() {
    if (prober_.joinable()) {
        prober_.request_stop();
        prober_.join();
    }
}

std::vector<EngineStatus> EnginePool::status() const {
    std::lock_guard lock(mutex_);
    std::vector<EngineStatus> out;
    out.reserve(engines_.size());
    for (const auto& e : engines_) {
        EngineStatus s;
        s.id = e->id;
        s.kind = e->backend->kind();
        s.endpoint = e->backend->endpoint();
        s.remote = e->backend->isRemote();
        s.healthy = e->healthy;
        s.enabled = e->enabled;
        s.pending = e->pending;
        s.completed = e->completed;
        s.failed = e->failed;
        s.consecutiveFailures = e->consecutiveFailures;
        s.latencyEwmaMs = e->latencyEwmaMs;
        s.lastError = e->lastError;
        s.models.assign(e->models.begin(), e->models.end());
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<std::string> EnginePool::models() const {
    std::set<std::string> all;
    {
        std::lock_guard lock(mutex_);
        for (const auto& e : engines_) {
            if (!e->enabled || !e->healthy) continue;
            if (e->backend->kind() == "echo") all.insert("echo");
            all.insert(e->models.begin(), e->models.end());
        }
    }
    return {all.begin(), all.end()};
}

ChatRequest PoolBackend::withDefaults(const ChatRequest& request) const {
    ChatRequest copy = request;
    if (copy.model.empty()) copy.model = defaultModel_;
    return copy;
}

ChatResult PoolBackend::chat(const ChatRequest& request) {
    return pool_->chat(withDefaults(request));
}

ChatResult PoolBackend::chatStream(const ChatRequest& request, const DeltaCallback& onDelta) {
    return pool_->chatStream(withDefaults(request), onDelta);
}

bool PoolBackend::listModels(std::vector<std::string>& out, std::string&) {
    out = pool_->models();
    return true;
}

std::string resolveEngineSpec(std::string_view spec) {
    struct Preset {
        std::string_view name;
        std::string_view hostPort;
    };
    static constexpr Preset presets[] = {{"ollama", "127.0.0.1:11434"},   {"llama-cpp", "127.0.0.1:8080"}, {"llamacpp", "127.0.0.1:8080"},
                                         {"llama.cpp", "127.0.0.1:8080"}, {"lmstudio", "127.0.0.1:1234"},  {"lm-studio", "127.0.0.1:1234"}};
    const auto semi = spec.find(';');
    const auto head = spec.substr(0, semi);
    const auto tail = semi == std::string_view::npos ? std::string_view() : spec.substr(semi);
    const auto at = head.find('@');
    const auto name = head.substr(0, at);
    for (const auto& p : presets) {
        if (name != p.name) continue;
        const auto hostPort = at == std::string_view::npos ? p.hostPort : head.substr(at + 1);
        return "http://" + std::string(hostPort) + "/v1" + std::string(tail);
    }
    return std::string(spec);
}

std::shared_ptr<Backend> makeBackend(std::string_view resolvedInput, std::string* error) {
    if (resolvedInput == "echo") return std::make_shared<EchoBackend>();
    const std::string resolved = resolveEngineSpec(resolvedInput);
    const std::string_view spec = resolved;

    ChatApiOptions options;
    std::string_view rest = spec;
    const auto semi = rest.find(';');
    options.baseUrl = std::string(rest.substr(0, semi));
    if (semi != std::string_view::npos) {
        rest.remove_prefix(semi + 1);
        while (!rest.empty()) {
            const auto next = rest.find(';');
            const auto pair = rest.substr(0, next);
            const auto eq = pair.find('=');
            if (eq != std::string_view::npos) {
                const auto key = pair.substr(0, eq);
                const auto value = std::string(pair.substr(eq + 1));
                if (key == "model")
                    options.defaultModel = value;
                else if (key == "key")
                    options.apiKey = value;
                else if (key == "peer" && value == "1") {
                    options.remote = true;
                    options.extraHeaders["X-Gygax-Forwarded"] = "1";
                } else {
                    if (error != nullptr) *error = "unknown engine option: " + std::string(key);
                    return nullptr;
                }
            }
            if (next == std::string_view::npos) break;
            rest.remove_prefix(next + 1);
        }
    }
    if (options.apiKey.empty()) {
        if (const char* env = std::getenv("GYGAX_ENGINE_API_KEY")) options.apiKey = env;
    }
    std::string urlError;
    if (!net::Url::parse(options.baseUrl, &urlError)) {
        if (error != nullptr) *error = "invalid engine spec '" + std::string(spec) + "': " + urlError;
        return nullptr;
    }
    return std::make_shared<ChatApiBackend>(std::move(options));
}

}
