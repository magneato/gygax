#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gygax/core/json.hpp>
#include <gygax/net/http.hpp>

namespace gygax::inference {

inline constexpr double kDefaultChatTemperature = 0.2;
inline constexpr std::chrono::milliseconds kDefaultBackendConnectTimeout{3000};
inline constexpr std::chrono::milliseconds kDefaultBackendReadTimeout{120000};
inline constexpr std::chrono::milliseconds kDefaultEngineProbeInterval{5000};
inline constexpr std::chrono::milliseconds kUnhealthyEngineProbeInterval{15000};
inline constexpr std::uint32_t kEngineFailuresBeforeUnhealthy = 2;
inline constexpr double kEngineLatencyEwmaAlpha = 0.3;

struct Message {
    std::string role;
    std::string content;
};

struct ChatRequest {
    std::string model;
    std::vector<Message> messages;
    double temperature = kDefaultChatTemperature;
    int maxTokens = 0;
    bool localOnly = false;
};

struct ChatResult {
    bool ok = false;
    bool retryable = false;
    int status = 0;
    std::string text;
    std::string model;
    std::string engine;
    std::string error;
    int promptTokens = 0;
    int completionTokens = 0;
    std::int64_t latencyMs = 0;
};

using DeltaCallback = std::function<bool(std::string_view delta)>;

std::string normalizeModel(std::string_view name);

class Backend {
public:
    virtual ~Backend() = default;
    [[nodiscard]] virtual std::string kind() const = 0;
    [[nodiscard]] virtual std::string endpoint() const = 0;
    [[nodiscard]] virtual bool acceptsAnyModel() const { return false; }
    [[nodiscard]] virtual bool isRemote() const { return false; }
    virtual ChatResult chat(const ChatRequest& request) = 0;
    virtual ChatResult chatStream(const ChatRequest& request, const DeltaCallback& onDelta);
    virtual bool listModels(std::vector<std::string>& out, std::string& error) = 0;
};

class EchoBackend final : public Backend {
public:
    static constexpr std::string_view kProtocolMarker = "GYGAX_AGENT_PROTOCOL";

    [[nodiscard]] std::string kind() const override { return "echo"; }
    [[nodiscard]] std::string endpoint() const override { return "builtin:echo"; }
    [[nodiscard]] bool acceptsAnyModel() const override { return true; }
    ChatResult chat(const ChatRequest& request) override;
    bool listModels(std::vector<std::string>& out, std::string& error) override;
};

struct ChatApiOptions {
    std::string baseUrl;
    std::string apiKey;
    std::string defaultModel;
    net::Headers extraHeaders;
    bool remote = false;
    std::chrono::milliseconds connectTimeout = kDefaultBackendConnectTimeout;
    std::chrono::milliseconds readTimeout = kDefaultBackendReadTimeout;
};

class ChatApiBackend final : public Backend {
public:
    explicit ChatApiBackend(ChatApiOptions options);

    [[nodiscard]] std::string kind() const override { return "chat-api"; }
    [[nodiscard]] std::string endpoint() const override { return options_.baseUrl; }
    [[nodiscard]] bool isRemote() const override { return options_.remote; }
    ChatResult chat(const ChatRequest& request) override;
    ChatResult chatStream(const ChatRequest& request, const DeltaCallback& onDelta) override;
    bool listModels(std::vector<std::string>& out, std::string& error) override;

private:
    [[nodiscard]] json::Value buildBody(const ChatRequest& request, bool stream) const;
    [[nodiscard]] net::Headers headers() const;
    [[nodiscard]] std::optional<net::Url> url(std::string_view suffix, std::string& error) const;

    ChatApiOptions options_;
};

struct EngineStatus {
    std::string id;
    std::string kind;
    std::string endpoint;
    bool remote = false;
    bool healthy = false;
    bool enabled = true;
    std::uint64_t pending = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::uint32_t consecutiveFailures = 0;
    double latencyEwmaMs = 0.0;
    std::string lastError;
    std::vector<std::string> models;
};

struct PoolOptions {
    std::chrono::milliseconds probeInterval = kDefaultEngineProbeInterval;
    std::chrono::milliseconds unhealthyProbeInterval = kUnhealthyEngineProbeInterval;
    std::uint32_t failuresBeforeUnhealthy = kEngineFailuresBeforeUnhealthy;
    double ewmaAlpha = kEngineLatencyEwmaAlpha;
};

class EnginePool {
public:
    explicit EnginePool(PoolOptions options = {});
    ~EnginePool();
    EnginePool(const EnginePool&) = delete;
    EnginePool& operator=(const EnginePool&) = delete;

    bool addEngine(std::string id, std::shared_ptr<Backend> backend, std::string* error = nullptr);
    bool removeEngine(const std::string& id);
    bool setEnabled(const std::string& id, bool enabled);
    void pin(std::optional<std::string> id);
    [[nodiscard]] std::optional<std::string> pinned() const;

    void startProbing();
    void stopProbing();
    void probeNow();

    ChatResult chat(const ChatRequest& request);
    ChatResult chatStream(const ChatRequest& request, const DeltaCallback& onDelta);

    [[nodiscard]] std::vector<EngineStatus> status() const;
    [[nodiscard]] std::vector<std::string> models() const;
    [[nodiscard]] std::size_t healthyCount() const;
    [[nodiscard]] std::size_t size() const;

private:
    struct Engine {
        std::string id;
        std::shared_ptr<Backend> backend;
        bool healthy = true;
        bool enabled = true;
        bool inventoryKnown = false;
        std::set<std::string> models;
        std::uint64_t pending = 0;
        std::uint64_t completed = 0;
        std::uint64_t failed = 0;
        std::uint32_t consecutiveFailures = 0;
        double latencyEwmaMs = 0.0;
        std::string lastError;
        std::chrono::steady_clock::time_point lastProbe{};
    };

    std::vector<std::string> rank(const ChatRequest& request) const;
    void record(const std::string& id, const ChatResult& result);
    void probeEngine(const std::string& id);
    void probeLoop(const std::stop_token& stop);

    PoolOptions options_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Engine>> engines_;
    std::optional<std::string> pinned_;
    std::jthread prober_;
    std::mutex probeMutex_;
    std::condition_variable_any probeWake_;
};

class PoolBackend final : public Backend {
public:
    explicit PoolBackend(std::shared_ptr<EnginePool> pool, std::string defaultModel = {})
        : pool_(std::move(pool)), defaultModel_(std::move(defaultModel)) {}

    [[nodiscard]] std::string kind() const override { return "pool"; }
    [[nodiscard]] std::string endpoint() const override { return "pool"; }
    ChatResult chat(const ChatRequest& request) override;
    ChatResult chatStream(const ChatRequest& request, const DeltaCallback& onDelta) override;
    bool listModels(std::vector<std::string>& out, std::string& error) override;

private:
    ChatRequest withDefaults(const ChatRequest& request) const;

    std::shared_ptr<EnginePool> pool_;
    std::string defaultModel_;
};

std::string resolveEngineSpec(std::string_view spec);
std::shared_ptr<Backend> makeBackend(std::string_view spec, std::string* error = nullptr);

}
