module;
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <gygax/core/object.hpp>
#include <gygax/core/singleton.hpp>

export module gygax.core.messaging;

export namespace gygax {

template <typename T, size_t Depth = 4> class Hashbelt {
public:
    void rotate() {
        std::lock_guard lock(mtx_);
        head_ = (head_ + 1) % Depth;
        belt_[head_].clear();
    }

    void put(const std::string& key, T val) {
        std::lock_guard lock(mtx_);
        belt_[head_][key].push_back(std::move(val));
    }

    std::vector<T> get_all_and_clear(const std::string& key) {
        std::lock_guard lock(mtx_);
        std::vector<T> combined;
        for (size_t i = 1; i <= Depth; ++i) {
            auto& map = belt_[(head_ + i) % Depth];
            if (auto it = map.find(key); it != map.end()) {
                combined.insert(combined.end(), std::make_move_iterator(it->second.begin()), std::make_move_iterator(it->second.end()));
                map.erase(it);
            }
        }
        return combined;
    }

private:
    std::array<std::unordered_map<std::string, std::vector<T>>, Depth> belt_;
    size_t head_ = 0;
    std::mutex mtx_;
};

struct BufferedMessage {
    std::string sender;
    std::string payload;
};

class Hub : public Singleton<Hub> {
    friend class Singleton<Hub>;

public:
    void registerObject(Object* obj) {
        std::vector<BufferedMessage> backlog;
        {
            std::lock_guard lock(mtx_);
            registry_[obj->name()] = obj;
            backlog = message_belt_.get_all_and_clear(obj->name());
        }
        for (const auto& m : backlog) obj->onMessage(m.sender, m.payload);
    }

    void unregisterObject(const std::string& name) {
        std::lock_guard lock(mtx_);
        registry_.erase(name);
    }

    [[nodiscard]] bool isRegistered(const std::string& name) const {
        std::lock_guard lock(mtx_);
        return registry_.contains(name);
    }

    void sendMessage(const std::string& sender, const std::string& target, const std::string& msg) {
        std::lock_guard lock(mtx_);
        if (auto it = registry_.find(target); it != registry_.end()) {
            it->second->onMessage(sender, msg);
        } else {
            message_belt_.put(target, BufferedMessage{sender, msg});
        }
    }

    void triggerEvent(const std::string& sender, const std::string& type, const std::string& data) {
        std::lock_guard lock(mtx_);
        std::vector<std::string> names;
        names.reserve(registry_.size());
        for (const auto& entry : registry_) {
            if (entry.first != sender) names.push_back(entry.first);
        }
        for (const auto& name : names) {
            if (auto it = registry_.find(name); it != registry_.end()) it->second->onEvent(type, sender, data);
        }
        event_belt_.put(type, BufferedMessage{sender, data});
    }

    std::vector<BufferedMessage> recentEvents(const std::string& type) {
        auto events = event_belt_.get_all_and_clear(type);
        for (const auto& e : events) event_belt_.put(type, e);
        return events;
    }

    void rotateBelt() {
        message_belt_.rotate();
        event_belt_.rotate();
    }

private:
    Hub() = default;

    mutable std::recursive_mutex mtx_;
    std::unordered_map<std::string, Object*> registry_;
    Hashbelt<BufferedMessage> message_belt_;
    Hashbelt<BufferedMessage> event_belt_;
};

}
