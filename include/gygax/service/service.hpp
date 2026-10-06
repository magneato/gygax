#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gygax/core/json.hpp>
#include <gygax/inference/backend.hpp>

namespace gygax::service {

inline constexpr std::uint16_t kDefaultServicePort = 1984;
inline constexpr std::size_t kDefaultServiceHttpWorkerCount = 16;
inline constexpr std::size_t kDefaultServiceAgentWorkerCount = 4;
inline constexpr std::uint32_t kDefaultServiceAgentMaxSteps = 8;
inline constexpr std::int64_t kDefaultServiceProbeIntervalMs = 5000;
inline constexpr std::int64_t kDefaultServiceAgentTimeoutMs = 120000;

struct ServiceConfig {
    std::string host = net::kDefaultHttpListenAddress;
    std::uint16_t port = kDefaultServicePort;
    std::string token;
    bool allowInsecureRemote = false;
    bool enableHttp = true;
    bool enableDeviceCommands = false;
    std::string ledgerPath;
    std::vector<std::string> plugins;
    std::vector<std::string> extensions;
    std::vector<std::string> mcpServers;
    std::vector<std::string> engines;
    std::vector<std::string> peers;
    std::string peerToken;
    std::string defaultModel;
    std::string toolProtocol = "json";
    std::string corsOrigin;
    std::size_t httpWorkers = kDefaultServiceHttpWorkerCount;
    std::size_t agentWorkers = kDefaultServiceAgentWorkerCount;
    std::uint32_t agentMaxSteps = kDefaultServiceAgentMaxSteps;
    std::size_t maxBodyBytes = net::kDefaultHttpBodyLimitBytes;
    std::size_t rateLimitPerMinute = 0;
    std::int64_t probeIntervalMs = kDefaultServiceProbeIntervalMs;
    std::int64_t agentTimeoutMs = kDefaultServiceAgentTimeoutMs;

    static ServiceConfig fromEnvironment();
    [[nodiscard]] bool validate(std::string& error) const;
};

bool isLoopbackHost(const std::string& host);
std::string readTokenFile(const std::string& path);

class Service {
public:
    explicit Service(ServiceConfig config);
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    bool start(std::string* error = nullptr);
    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] const ServiceConfig& config() const;
    [[nodiscard]] inference::EnginePool& pool();

    [[nodiscard]] json::Value rpc(const json::Value& request);
    [[nodiscard]] std::string metricsText();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
