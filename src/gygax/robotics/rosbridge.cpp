#include <gygax/robotics/rosbridge.hpp>

#include <cerrno>
#include <format>

namespace gygax::robotics {

RosbridgeClient::RosbridgeClient(std::unique_ptr<net::WebSocketClient> socket) : socket_(std::move(socket)) {}

RosbridgeClient::~RosbridgeClient() {
    stop();
}

std::unique_ptr<RosbridgeClient> RosbridgeClient::connect(std::string_view url, std::chrono::milliseconds timeout, std::string* error) {
    auto socket = net::WebSocketClient::connect(url, timeout, error);
    if (!socket) return nullptr;
    return std::make_unique<RosbridgeClient>(std::move(socket));
}

void RosbridgeClient::start() {
    if (thread_.joinable()) return;
    thread_ = std::jthread([this](const std::stop_token& st) { loop(st); });
}

void RosbridgeClient::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    if (socket_) (void)socket_->close();
    connected_ = false;
}

void RosbridgeClient::loop(const std::stop_token& stop) {
    while (!stop.stop_requested()) {
        net::WsMessage m;
        const auto rc = socket_->receive(m, std::chrono::milliseconds(100));
        if (rc == -ETIMEDOUT) continue;
        if (rc != 0) {
            connected_ = false;
            std::lock_guard lock(mutex_);
            lastError_ = "connection lost";
            responses_.notify_all();
            return;
        }
        if (m.opcode != net::WsOpcode::Text) continue;
        auto parsed = json::parse(m.payload);
        if (!parsed || !parsed->isObject()) continue;
        const auto op = parsed->getString("op");
        TopicHandler handler;
        json::Value message;
        std::string topic;
        {
            std::lock_guard lock(mutex_);
            if (op == "publish") {
                topic = parsed->getString("topic");
                if (const auto* msg = parsed->find("msg")) {
                    message = *msg;
                    last_[topic] = message;
                    if (auto it = handlers_.find(topic); it != handlers_.end()) handler = it->second;
                }
            } else if (op == "service_response") {
                serviceResults_[parsed->getString("id")] = *parsed;
                responses_.notify_all();
            } else if (op == "status") {
                if (parsed->getString("level") == "error") lastError_ = parsed->getString("msg");
            }
        }
        if (handler) handler(topic, message);
    }
}

net::IOResult RosbridgeClient::send(const json::Value& message) {
    if (!connected_.load()) return -ECONNRESET;
    return socket_->sendText(message.dump());
}

net::IOResult RosbridgeClient::advertise(const std::string& topic, const std::string& type) {
    if (topic.empty() || type.empty()) return -EINVAL;
    json::Value m = json::Value::object();
    m["op"] = "advertise";
    m["topic"] = topic;
    m["type"] = type;
    return send(m);
}

net::IOResult RosbridgeClient::unadvertise(const std::string& topic) {
    json::Value m = json::Value::object();
    m["op"] = "unadvertise";
    m["topic"] = topic;
    return send(m);
}

net::IOResult RosbridgeClient::publish(const std::string& topic, const json::Value& message) {
    if (topic.empty() || !message.isObject()) return -EINVAL;
    json::Value m = json::Value::object();
    m["op"] = "publish";
    m["topic"] = topic;
    m["msg"] = message;
    return send(m);
}

net::IOResult RosbridgeClient::subscribe(const std::string& topic, const std::string& type, TopicHandler handler, int throttleMs) {
    if (topic.empty()) return -EINVAL;
    {
        std::lock_guard lock(mutex_);
        handlers_[topic] = std::move(handler);
    }
    json::Value m = json::Value::object();
    m["op"] = "subscribe";
    m["topic"] = topic;
    if (!type.empty()) m["type"] = type;
    if (throttleMs > 0) m["throttle_rate"] = throttleMs;
    return send(m);
}

net::IOResult RosbridgeClient::unsubscribe(const std::string& topic) {
    {
        std::lock_guard lock(mutex_);
        handlers_.erase(topic);
    }
    json::Value m = json::Value::object();
    m["op"] = "unsubscribe";
    m["topic"] = topic;
    return send(m);
}

net::IOResult RosbridgeClient::callService(const std::string& service, const json::Value& args, json::Value& result,
                                           std::chrono::milliseconds timeout) {
    if (service.empty()) return -EINVAL;
    const std::string id = std::format("call_service:{}:{}", service, ++counter_);
    json::Value m = json::Value::object();
    m["op"] = "call_service";
    m["service"] = service;
    m["id"] = id;
    if (args.isObject()) m["args"] = args;
    if (const auto rc = send(m); rc != 0) return rc;
    std::unique_lock lock(mutex_);
    if (!responses_.wait_for(lock, timeout, [&] { return serviceResults_.contains(id) || !connected_.load(); })) return -ETIMEDOUT;
    auto it = serviceResults_.find(id);
    if (it == serviceResults_.end()) return -ECONNRESET;
    const json::Value response = std::move(it->second);
    serviceResults_.erase(it);
    if (!response.getBool("result", true)) {
        const auto* values = response.find("values");
        lastError_ = values != nullptr && values->isString() ? values->asString() : "service call failed";
        return -EREMOTEIO;
    }
    const auto* values = response.find("values");
    result = values != nullptr ? *values : json::Value::object();
    return 0;
}

net::IOResult RosbridgeClient::publishTwist(const std::string& topic, double linearX, double angularZ) {
    json::Value twist = json::Value::object();
    twist["linear"]["x"] = linearX;
    twist["linear"]["y"] = 0.0;
    twist["linear"]["z"] = 0.0;
    twist["angular"]["x"] = 0.0;
    twist["angular"]["y"] = 0.0;
    twist["angular"]["z"] = angularZ;
    return publish(topic, twist);
}

std::optional<json::Value> RosbridgeClient::lastMessage(const std::string& topic) const {
    std::lock_guard lock(mutex_);
    auto it = last_.find(topic);
    if (it == last_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::string> RosbridgeClient::subscribedTopics() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.reserve(handlers_.size());
    for (const auto& [topic, h] : handlers_) out.push_back(topic);
    return out;
}

std::string RosbridgeClient::lastError() const {
    std::lock_guard lock(mutex_);
    return lastError_;
}

} // namespace gygax::robotics
