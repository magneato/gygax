module;
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <format>
#include <cmath>
#include <gygax/core/log.hpp>

export module gygax.state_machine;

import gygax.core.base;
import gygax.tools;
import gygax.world_model;

namespace {
constexpr float kFloatComparisonTolerance = 1e-5f;
}

export namespace gygax::state_machine {

class Action {
public:
    virtual ~Action() = default;
    virtual void execute(BaseObject* obj) = 0;
};

class ToolAction : public Action {
public:
    ToolAction(std::string toolName, std::string payload) : toolName_(std::move(toolName)), payload_(std::move(payload)) {}

    void execute(BaseObject* obj) override {
        log::debug("state_machine", "tool action '{}' on '{}'", toolName_, obj->name());
        auto result = tools::ToolRegistry::getInstance().execute(toolName_, payload_);
        if (result) {
            log::debug("state_machine", "tool '{}' returned {} bytes", toolName_, result->size());
        } else {
            log::warn("state_machine", "tool '{}' is not registered", toolName_);
        }
    }

private:
    std::string toolName_;
    std::string payload_;
};

class LambdaAction : public Action {
public:
    LambdaAction(std::function<void(BaseObject*)> func) : func_(std::move(func)) {}
    void execute(BaseObject* obj) override {
        if (func_) func_(obj);
    }

private:
    std::function<void(BaseObject*)> func_;
};

class Condition {
public:
    virtual ~Condition() = default;
    virtual bool evaluate(BaseObject* obj) = 0;
};

class LambdaCondition : public Condition {
public:
    LambdaCondition(std::function<bool(BaseObject*)> func) : func_(std::move(func)) {}
    bool evaluate(BaseObject* obj) override { return func_ ? func_(obj) : false; }

private:
    std::function<bool(BaseObject*)> func_;
};

class ParameterThresholdCondition : public Condition {
public:
    enum class Op { Equal, GreaterThan, LessThan };
    ParameterThresholdCondition(std::string key, float threshold, Op op) : key_(std::move(key)), threshold_(threshold), op_(op) {}

    bool evaluate(BaseObject* obj) override {
        auto* val = obj->getParameter(key_);
        if (!val) return false;

        float current = 0.0f;
        if (std::holds_alternative<float>(*val)) {
            current = std::get<float>(*val);
        } else if (std::holds_alternative<int>(*val)) {
            current = static_cast<float>(std::get<int>(*val));
        } else {
            return false;
        }

        switch (op_) {
        case Op::Equal: return std::abs(current - threshold_) < kFloatComparisonTolerance;
        case Op::GreaterThan: return current >= threshold_;
        case Op::LessThan: return current <= threshold_;
        }
        return false;
    }

private:
    std::string key_;
    float threshold_;
    Op op_;
};

struct Transition {
    std::shared_ptr<Condition> condition;
    std::string targetState;
    std::vector<std::shared_ptr<Action>> onTransitionActions;
};

class State {
public:
    State(std::string name) : name_(std::move(name)) {}

    const std::string& name() const { return name_; }

    void addEntryAction(std::shared_ptr<Action> action) { entryActions_.push_back(std::move(action)); }
    void addExitAction(std::shared_ptr<Action> action) { exitActions_.push_back(std::move(action)); }
    void addTransition(std::shared_ptr<Condition> cond, std::string targetState, std::vector<std::shared_ptr<Action>> actions = {}) {
        transitions_.push_back(Transition{std::move(cond), std::move(targetState), std::move(actions)});
    }

    const std::vector<std::shared_ptr<Action>>& entryActions() const { return entryActions_; }
    const std::vector<std::shared_ptr<Action>>& exitActions() const { return exitActions_; }
    const std::vector<Transition>& transitions() const { return transitions_; }

private:
    std::string name_;
    std::vector<std::shared_ptr<Action>> entryActions_;
    std::vector<std::shared_ptr<Action>> exitActions_;
    std::vector<Transition> transitions_;
};

class StateMachine {
public:
    StateMachine() : active_(false), transitionsCount_(0) {}

    void addState(std::shared_ptr<State> state) {
        const std::string key = state->name();
        states_[key] = std::move(state);
    }

    void setInitialState(std::string name) { initialState_ = std::move(name); }

    void addCompletionAction(std::shared_ptr<Action> action) { completionActions_.push_back(std::move(action)); }

    void start(BaseObject* obj) {
        if (states_.empty()) return;
        std::string startState = initialState_.empty() ? states_.begin()->first : initialState_;
        if (!states_.contains(startState)) return;
        auto wmState = state::WorldModel::getInstance().getRepresentation<state::StateMachineState>(obj->sid());
        if (wmState && wmState->active && states_.contains(wmState->currentState)) {
            active_ = true;
            currentState_ = states_[wmState->currentState];
            transitionsCount_ = wmState->transitionsCount;
            log::info("state_machine", "resumed '{}' in state '{}'", obj->name(), currentState_->name());
            return;
        }

        active_ = true;
        currentState_ = states_[startState];
        transitionsCount_ = 0;
        log::debug("state_machine", "starting '{}' in state '{}'", obj->name(), currentState_->name());

        persistState(obj);

        for (auto& action : currentState_->entryActions()) {
            action->execute(obj);
        }
    }

    void step(BaseObject* obj) {
        if (!active_ || !currentState_) return;

        for (auto& trans : currentState_->transitions()) {
            if (trans.condition && trans.condition->evaluate(obj)) {
                log::debug("state_machine", "'{}' -> '{}'", currentState_->name(), trans.targetState);

                for (auto& action : currentState_->exitActions()) {
                    action->execute(obj);
                }

                for (auto& action : trans.onTransitionActions) {
                    action->execute(obj);
                }

                transitionsCount_++;
                auto nextStateIt = states_.find(trans.targetState);
                if (nextStateIt == states_.end()) {
                    log::debug("state_machine", "terminal transition to '{}'; stopping machine", trans.targetState);
                    active_ = false;
                    currentState_ = nullptr;
                    persistState(obj);
                    executeCompletion(obj);
                    return;
                }

                currentState_ = nextStateIt->second;
                persistState(obj);

                for (auto& action : currentState_->entryActions()) {
                    action->execute(obj);
                }

                return;
            }
        }
    }

    void stop(BaseObject* obj) {
        if (!active_) return;
        active_ = false;
        if (currentState_) {
            for (auto& action : currentState_->exitActions()) {
                action->execute(obj);
            }
        }
        currentState_ = nullptr;
        persistState(obj);
        executeCompletion(obj);
    }

    bool isActive() const { return active_; }
    std::shared_ptr<State> currentState() const { return currentState_; }
    uint32_t transitionsCount() const { return transitionsCount_; }

private:
    void executeCompletion(BaseObject* obj) {
        log::debug("state_machine", "completed '{}'", obj->name());
        for (auto& action : completionActions_) {
            action->execute(obj);
        }
    }

    void persistState(BaseObject* obj) {
        state::StateMachineState sms;
        sms.currentState = currentState_ ? currentState_->name() : "";
        sms.active = active_;
        sms.transitionsCount = transitionsCount_;
        state::WorldModel::getInstance().updateRepresentation<state::StateMachineState>(obj->sid(), std::move(sms));
    }

    std::unordered_map<std::string, std::shared_ptr<State>> states_;
    std::string initialState_;
    std::shared_ptr<State> currentState_;
    std::vector<std::shared_ptr<Action>> completionActions_;
    bool active_;
    uint32_t transitionsCount_;
};

}
