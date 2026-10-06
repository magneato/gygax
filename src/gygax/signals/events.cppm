module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <gygax/core/singleton.hpp>

export module gygax.signals;

export namespace gygax::signals {

struct UserObjective {
    uint32_t sid;
    std::string query;
};

struct ModelPrediction {
    uint32_t sid;
    std::string rawJson;
};

struct ActionCall {
    uint32_t sid;
    std::string toolName;
    std::string payload;
};

struct WebConstructionRequest {
    std::string objective;
};

using UserQuerySignal = UserObjective;
using InferenceResponseSignal = ModelPrediction;
using TaskExecutionSignal = ActionCall;

using InternalSignal = std::variant<UserObjective, ModelPrediction, ActionCall, WebConstructionRequest>;

class Subscription {
public:
    explicit Subscription(std::size_t capacity) : capacity_(capacity) {}

    void deliver(const InternalSignal& signal) {
        {
            std::lock_guard lock(mutex_);
            if (queue_.size() >= capacity_) {
                queue_.pop_front();
                dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            queue_.push_back(signal);
        }
        ready_.notify_one();
    }

    std::optional<InternalSignal> poll() {
        std::lock_guard lock(mutex_);
        return popLocked();
    }

    template <typename Rep, typename Period> std::optional<InternalSignal> waitFor(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock lock(mutex_);
        ready_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; });
        return popLocked();
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }

    [[nodiscard]] uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    [[nodiscard]] std::size_t pending() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    std::optional<InternalSignal> popLocked() {
        if (queue_.empty()) return std::nullopt;
        InternalSignal value = std::move(queue_.front());
        queue_.pop_front();
        return value;
    }

    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<InternalSignal> queue_;
    bool closed_ = false;
    std::atomic<uint64_t> dropped_{0};
};

class GlobalEventRelay : public ::gygax::Singleton<GlobalEventRelay> {
    friend class ::gygax::Singleton<GlobalEventRelay>;

public:
    static constexpr std::size_t kDefaultCapacity = 4096;

    static std::shared_ptr<Subscription> subscribe(std::size_t capacity = kDefaultCapacity) {
        auto& self = getInstance();
        auto sub = std::make_shared<Subscription>(capacity);
        std::lock_guard lock(self.mutex_);
        self.subscribers_.push_back(sub);
        return sub;
    }

    static void unsubscribe(const std::shared_ptr<Subscription>& sub) {
        auto& self = getInstance();
        std::lock_guard lock(self.mutex_);
        std::erase_if(self.subscribers_, [&](const std::weak_ptr<Subscription>& w) {
            auto locked = w.lock();
            return !locked || locked == sub;
        });
        sub->close();
    }

    static void Broadcast(const InternalSignal& sig) {
        auto& self = getInstance();
        std::vector<std::shared_ptr<Subscription>> targets;
        {
            std::lock_guard lock(self.mutex_);
            std::erase_if(self.subscribers_, [](const std::weak_ptr<Subscription>& w) { return w.expired(); });
            for (const auto& w : self.subscribers_) {
                if (auto s = w.lock()) targets.push_back(std::move(s));
            }
        }
        for (const auto& s : targets) s->deliver(sig);
        self.published_.fetch_add(1, std::memory_order_relaxed);
    }

    static uint64_t published() { return getInstance().published_.load(std::memory_order_relaxed); }

private:
    GlobalEventRelay() = default;

    std::mutex mutex_;
    std::vector<std::weak_ptr<Subscription>> subscribers_;
    std::atomic<uint64_t> published_{0};
};

}
