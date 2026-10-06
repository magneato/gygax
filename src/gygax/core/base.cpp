#include <gygax/core/object.hpp>

#include <algorithm>
#include <format>
#include <numeric>
#include <string>
#include <vector>

import gygax.core.messaging;
import gygax.world_model;

namespace gygax {

namespace {

constexpr std::size_t kThoughtSummaryRecentCount = 3;
constexpr std::size_t kThoughtSummaryEntryMaxLength = 80;

std::string clip(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) return text;
    return text.substr(0, limit) + "...";
}

}

BaseObject::BaseObject(std::string name, Capability caps)
    : name_(std::move(name)), caps_(caps), frozen_(false), maxThoughts_(DEFAULT_MAX_THOUGHTS) {
    sid_ = state::WorldModel::getInstance().spawnAgent();
}

void BaseObject::setParameter(const std::string& key, Parameter val) {
    if (frozen_) return;
    params_[key] = std::move(val);
}

const BaseObject::Parameter* BaseObject::getParameter(const std::string& key) const {
    if (auto it = params_.find(key); it != params_.end()) return &it->second;
    return nullptr;
}

void BaseObject::sendMessage(const std::string& target, const std::string& msg) {
    Hub::getInstance().sendMessage(name_, target, msg);
}

void BaseObject::triggerEvent(const std::string& type, const std::string& data) {
    Hub::getInstance().triggerEvent(name_, type, data);
}

void BaseObject::initialize() {
    Hub::getInstance().registerObject(this);
    log::debug(name_, "initialized and registered");
}

void BaseObject::pre_delete() {
    Hub::getInstance().unregisterObject(name_);
    log::debug(name_, "unregistered");
}

void BaseObject::pushThought(const std::string& context) {
    thoughts_.push_back(context);
    if (maxThoughts_ > 0 && thoughts_.size() > maxThoughts_) {
        std::string summary = summarizeThoughts();
        thoughts_.clear();
        thoughts_.push_back(std::format("Summary of previous context: {}", summary));
        log::debug(name_, "thought buffer compacted");
    }
}

std::string BaseObject::summarizeThoughts() const {
    std::string goalList = std::accumulate(goals_.begin(), goals_.end(), std::string(),
                                           [](const std::string& a, const std::string& b) { return a + (a.empty() ? "" : ", ") + b; });
    std::string recent;
    const std::size_t from = thoughts_.size() > kThoughtSummaryRecentCount
                                 ? thoughts_.size() - kThoughtSummaryRecentCount
                                 : 0;
    for (std::size_t i = from; i < thoughts_.size(); ++i)
        recent += (recent.empty() ? "" : " | ") + clip(thoughts_[i], kThoughtSummaryEntryMaxLength);
    return std::format("{} thoughts; goals [{}]; latest: {}", thoughts_.size(), goalList.empty() ? "none" : goalList,
                       recent.empty() ? "none" : recent);
}

}
