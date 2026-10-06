module;
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <gygax/core/agent.hpp>

export module gygax.nodes.ether;

import gygax.world_model;
import gygax.messaging;
import gygax.core.base;

export namespace gygax::nodes {

class EtherAgentBrain : public gygax::AbstractAgent, public gygax::BaseObject {
public:
    explicit EtherAgentBrain(state::sid_t id) : BaseObject("EtherBrain", Capability::Cognition | Capability::Communication), sid_(id) {}

    [[nodiscard]] uint32_t sid() const override { return sid_; }

    [[nodiscard]] bool receiveMessage(const messaging::Message& msg) override {
        return remember(std::format("message from {}: {}", msg.senderId, msg.payload));
    }

    void emitEvent(uint32_t id, const void*, size_t size) override { remember(std::format("event {} ({} bytes)", id, size)); }

    void initialize() override { BaseObject::initialize(); }
    void freeze(bool f) override { BaseObject::freeze(f); }
    void shutdown() override { BaseObject::shutdown(); }
    void pre_delete() override { BaseObject::pre_delete(); }
    void reset() override { BaseObject::reset(); }
    Capability capabilities() const override { return BaseObject::capabilities(); }
    const std::string& name() const override { return BaseObject::name(); }
    bool compatible(Capability r) const override { return BaseObject::compatible(r); }
    bool compatible(const Object& o) const override { return BaseObject::compatible(o); }
    std::string diff(const Object& o) const override { return BaseObject::diff(o); }

private:
    bool remember(std::string entry) {
        return state::WorldModel::getInstance().mutate<state::EpisodicMemory>(
            sid_, [&](state::EpisodicMemory& m) { m.append(std::move(entry)); });
    }

    state::sid_t sid_;
};

}

namespace gygax {

extern "C++" {

std::unique_ptr<gygax::AbstractAgent> CreateDefaultAgent() {
    auto& world = state::WorldModel::getInstance();
    const state::sid_t id = world.spawnAgent();
    world.updateRepresentation(id, state::EpisodicMemory{{}, "default research agent", state::kDefaultEpisodicMemoryCapacity});
    world.updateRepresentation(id, state::LatentSpace{});
    return std::make_unique<gygax::nodes::EtherAgentBrain>(id);
}
}

}
