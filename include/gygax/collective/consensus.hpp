#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gygax/core/singleton.hpp>

namespace gygax {

struct Hypothesis {
    uint32_t originatorSid;
    std::string content;
    float probability;
};

struct ConsensusEntry {
    std::string content;
    double confidence = 0.0;
    std::size_t votes = 0;
};

class CollectiveBrain : public Singleton<CollectiveBrain> {
    friend class Singleton<CollectiveBrain>;

public:
    void submitHypothesis(const Hypothesis& h) {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_.push_back({h.originatorSid, h.content, std::clamp(h.probability, 0.0F, 1.0F)});
    }

    [[nodiscard]] std::vector<Hypothesis> getPool() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pool_;
    }

    [[nodiscard]] std::vector<ConsensusEntry> report() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, ConsensusEntry> merged;
        for (const auto& h : pool_) {
            auto& entry = merged[h.content];
            entry.content = h.content;
            entry.confidence = 1.0 - (1.0 - entry.confidence) * (1.0 - static_cast<double>(h.probability));
            ++entry.votes;
        }
        std::vector<ConsensusEntry> out;
        out.reserve(merged.size());
        for (auto& [content, entry] : merged) out.push_back(std::move(entry));
        std::ranges::sort(out, [](const ConsensusEntry& a, const ConsensusEntry& b) {
            if (a.confidence != b.confidence) return a.confidence > b.confidence;
            return a.content < b.content;
        });
        return out;
    }

    [[nodiscard]] std::string deriveConsensus() const {
        const auto ranked = report();
        return ranked.empty() ? "IDLE" : ranked.front().content;
    }

    void resetPool() {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_.clear();
    }

private:
    CollectiveBrain() = default;
    std::vector<Hypothesis> pool_;
    mutable std::mutex mutex_;
};

} // namespace gygax
