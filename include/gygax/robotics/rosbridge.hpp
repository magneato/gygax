#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <gygax/core/json.hpp>
#include <gygax/net/websocket.hpp>

namespace gygax::robotics {

inline constexpr std::chrono::milliseconds kDefaultRosbridgeServiceTimeout{3000};

class RosbridgeClient {
public:
    using TopicHandler = std::function<void(const std::string& topic, const json::Value& message)>;

    explicit RosbridgeClient(std::unique_ptr<net::WebSocketClient> socket);
    ~RosbridgeClient();
    RosbridgeClient(const RosbridgeClient&) = delete;
    RosbridgeClient& operator=(const RosbridgeClient&) = delete;

    static std::unique_ptr<RosbridgeClient> connect(std::string_view url, std::chrono::milliseconds timeout, std::string* error = nullptr);

    void start();
    void stop();
    [[nodiscard]] bool connected() const { return connected_.load(); }

    [[nodiscard]] net::IOResult advertise(const std::string& topic, const std::string& type);
    [[nodiscard]] net::IOResult unadvertise(const std::string& topic);
    [[nodiscard]] net::IOResult publish(const std::string& topic, const json::Value& message);
    [[nodiscard]] net::IOResult subscribe(const std::string& topic, const std::string& type = {}, TopicHandler handler = nullptr,
                                          int throttleMs = 0);
    [[nodiscard]] net::IOResult unsubscribe(const std::string& topic);
    [[nodiscard]] net::IOResult callService(const std::string& service, const json::Value& args, json::Value& result,
                                            std::chrono::milliseconds timeout = kDefaultRosbridgeServiceTimeout);
    [[nodiscard]] net::IOResult publishTwist(const std::string& topic, double linearX, double angularZ);

    [[nodiscard]] std::optional<json::Value> lastMessage(const std::string& topic) const;
    [[nodiscard]] std::vector<std::string> subscribedTopics() const;
    [[nodiscard]] std::string lastError() const;

private:
    void loop(const std::stop_token& stop);
    net::IOResult send(const json::Value& message);

    std::unique_ptr<net::WebSocketClient> socket_;
    mutable std::mutex mutex_;
    std::condition_variable responses_;
    std::map<std::string, json::Value> last_;
    std::map<std::string, TopicHandler> handlers_;
    std::map<std::string, json::Value> serviceResults_;
    std::string lastError_;
    std::atomic<std::uint64_t> counter_{0};
    std::atomic<bool> connected_{true};
    std::jthread thread_;
};

} // namespace gygax::robotics
