module;
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <gygax/core/log.hpp>
#include <gygax/core/singleton.hpp>

export module gygax.world_model;

export namespace gygax::state {

using sid_t = uint32_t;

inline constexpr sid_t kFirstAgentSid = 1000;
inline constexpr uint32_t kDefaultEpisodicMemoryCapacity = 16384;
inline constexpr uint32_t kDefaultAgentMaxSteps = 20;

struct EpisodicMemory {
    std::vector<std::string> eventLogs;
    std::string heuristicBase;
    uint32_t capacity = kDefaultEpisodicMemoryCapacity;

    void append(std::string entry) {
        eventLogs.push_back(std::move(entry));
        if (capacity > 0 && eventLogs.size() > capacity) {
            const auto excess = eventLogs.size() - capacity;
            eventLogs.erase(eventLogs.begin(), eventLogs.begin() + static_cast<std::ptrdiff_t>(excess));
        }
    }
};

enum class RunStatus { Idle, Running, Completed, Failed };

[[nodiscard]] constexpr const char* toString(RunStatus s) {
    switch (s) {
    case RunStatus::Idle: return "idle";
    case RunStatus::Running: return "running";
    case RunStatus::Completed: return "completed";
    case RunStatus::Failed: return "failed";
    }
    return "idle";
}

struct LatentSpace {
    std::string objective;
    std::string answer;
    std::string error;
    RunStatus status = RunStatus::Idle;
    uint32_t stepCount = 0;
    uint32_t maxSteps = kDefaultAgentMaxSteps;
    uint64_t runId = 0;
};

struct StateMachineState {
    std::string currentState;
    bool active = false;
    uint32_t transitionsCount = 0;
};

struct Swarm {
    uint32_t swarmId;
    std::string objective;
    std::unordered_set<sid_t> activeAgents;
    std::vector<std::string> globalKnowledgeBase;
};

class WorldModel : public Singleton<WorldModel> {
    friend class Singleton<WorldModel>;

public:
    template <typename T> [[nodiscard]] std::shared_ptr<T> getRepresentation(sid_t id) {
        std::shared_lock lock(mutex_);
        auto& table = tableFor<T>();
        if (auto it = table.find(id); it != table.end()) return it->second;
        return nullptr;
    }

    template <typename T> [[nodiscard]] std::optional<T> snapshot(sid_t id) const {
        std::shared_lock lock(mutex_);
        const auto& table = tableFor<T>();
        if (auto it = table.find(id); it != table.end()) return *it->second;
        return std::nullopt;
    }

    template <typename T, typename Fn> bool mutate(sid_t id, Fn&& fn) {
        std::unique_lock lock(mutex_);
        auto& table = tableFor<T>();
        auto it = table.find(id);
        if (it == table.end()) return false;
        std::forward<Fn>(fn)(*it->second);
        return true;
    }

    template <typename T> void updateRepresentation(sid_t id, T component) {
        std::unique_lock lock(mutex_);
        tableFor<T>()[id] = std::make_shared<T>(std::move(component));
    }

    template <typename T> void removeRepresentation(sid_t id) {
        std::unique_lock lock(mutex_);
        tableFor<T>().erase(id);
    }

    sid_t spawnAgent() { return nextSid_.fetch_add(1, std::memory_order_relaxed); }

    uint32_t spawnSwarm(const std::string& objective) {
        std::unique_lock lock(mutex_);
        const uint32_t id = nextSwarmId_++;
        swarms_[id] = std::make_shared<Swarm>(Swarm{id, objective, {}, {}});
        return id;
    }

    bool assignAgentToSwarm(sid_t agentId, uint32_t swarmId) {
        std::unique_lock lock(mutex_);
        auto it = swarms_.find(swarmId);
        if (it == swarms_.end()) return false;
        it->second->activeAgents.insert(agentId);
        return true;
    }

    bool addKnowledge(uint32_t swarmId, std::string fact) {
        std::unique_lock lock(mutex_);
        auto it = swarms_.find(swarmId);
        if (it == swarms_.end()) return false;
        it->second->globalKnowledgeBase.push_back(std::move(fact));
        return true;
    }

    [[nodiscard]] std::optional<Swarm> swarmSnapshot(uint32_t swarmId) const {
        std::shared_lock lock(mutex_);
        auto it = swarms_.find(swarmId);
        if (it == swarms_.end()) return std::nullopt;
        return *it->second;
    }

    bool mergeSwarms(uint32_t hostSwarmId, uint32_t guestSwarmId) {
        if (hostSwarmId == guestSwarmId) return false;
        std::unique_lock lock(mutex_);
        auto hostIt = swarms_.find(hostSwarmId);
        auto guestIt = swarms_.find(guestSwarmId);
        if (hostIt == swarms_.end() || guestIt == swarms_.end()) return false;

        auto& hostKb = hostIt->second->globalKnowledgeBase;
        const auto& guestKb = guestIt->second->globalKnowledgeBase;
        hostKb.reserve(hostKb.size() + guestKb.size());
        hostKb.insert(hostKb.end(), guestKb.begin(), guestKb.end());
        std::ranges::sort(hostKb);
        hostKb.erase(std::unique(hostKb.begin(), hostKb.end()), hostKb.end());

        hostIt->second->activeAgents.insert(guestIt->second->activeAgents.begin(), guestIt->second->activeAgents.end());
        log::info("world", "swarm {} merged into swarm {}", guestSwarmId, hostSwarmId);
        return true;
    }

private:
    WorldModel() = default;

    template <typename T> auto& tableFor() {
        if constexpr (std::is_same_v<T, EpisodicMemory>) {
            return memories_;
        } else if constexpr (std::is_same_v<T, LatentSpace>) {
            return spaces_;
        } else {
            static_assert(std::is_same_v<T, StateMachineState>, "unsupported world representation");
            return stateMachines_;
        }
    }

    template <typename T> const auto& tableFor() const {
        if constexpr (std::is_same_v<T, EpisodicMemory>) {
            return memories_;
        } else if constexpr (std::is_same_v<T, LatentSpace>) {
            return spaces_;
        } else {
            static_assert(std::is_same_v<T, StateMachineState>, "unsupported world representation");
            return stateMachines_;
        }
    }

    std::atomic<sid_t> nextSid_{kFirstAgentSid};
    uint32_t nextSwarmId_ = 1;
    std::unordered_map<sid_t, std::shared_ptr<EpisodicMemory>> memories_;
    std::unordered_map<sid_t, std::shared_ptr<LatentSpace>> spaces_;
    std::unordered_map<sid_t, std::shared_ptr<StateMachineState>> stateMachines_;
    std::unordered_map<uint32_t, std::shared_ptr<Swarm>> swarms_;
    mutable std::shared_mutex mutex_;
};

}
