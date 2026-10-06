module;
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <variant>
#include <format>
#include <chrono>
#include <queue>
#include <cstdint>
#include <gygax/core/json.hpp>
#include <gygax/core/log.hpp>

export module gygax.hardware.core;
import gygax.core.base;
import gygax.state_machine;

export namespace gygax::hardware {
enum class Medium {
    Any,
    Electrical,
    Pneumatic,
    Hydraulic,
    Optical,
    Data_Ethernet,
    Data_Fiber,
    Data_LadderLogic,
    Mechanical,
    Thermal,
    Fluid_Water,
    Quantum_Entanglement,
    Acoustic
};

inline std::string_view mediumToString(Medium m) {
    switch (m) {
    case Medium::Electrical: return "Electrical";
    case Medium::Pneumatic: return "Pneumatic";
    case Medium::Hydraulic: return "Hydraulic";
    case Medium::Optical: return "Optical";
    case Medium::Data_Ethernet: return "Data_Ethernet";
    case Medium::Data_Fiber: return "Data_Fiber";
    case Medium::Data_LadderLogic: return "Data_LadderLogic";
    case Medium::Mechanical: return "Mechanical";
    case Medium::Thermal: return "Thermal";
    case Medium::Fluid_Water: return "Fluid_Water";
    case Medium::Quantum_Entanglement: return "Quantum_Entanglement";
    case Medium::Acoustic: return "Acoustic";
    default: return "Any/Unknown";
    }
}

class Node;
class Link;
class Interface {
public:
    Interface(std::string name, Medium medium, Node* owner) : name_(std::move(name)), medium_(medium), owner_(owner), link_(nullptr) {}

    const std::string& name() const { return name_; }
    Medium medium() const { return medium_; }
    Node* owner() const { return owner_; }
    bool isConnected() const { return link_ != nullptr; }

    void connect(Link* l) { link_ = l; }
    void disconnect() { link_ = nullptr; }

private:
    std::string name_;
    Medium medium_;
    Node* owner_;
    Link* link_;
};
class Link : public BaseObject {
public:
    Link(std::string name, Medium medium) : BaseObject(std::move(name)), medium_(medium), a_(nullptr), b_(nullptr) {}

    Medium medium() const { return medium_; }

    bool bind(Interface* p1, Interface* p2) {
        if (frozen_) return false;
        if (!p1 || !p2) return false;
        if (p1->medium() != medium_ && p1->medium() != Medium::Any) return false;
        if (p2->medium() != medium_ && p2->medium() != Medium::Any) return false;

        a_ = p1;
        b_ = p2;
        p1->connect(this);
        p2->connect(this);
        log::debug("link", "{} ({}) established between {} and {}", name_, mediumToString(medium_), p1->name(), p2->name());
        return true;
    }

private:
    Medium medium_;
    Interface *a_, *b_;
};

class Node : public BaseObject {
public:
    using Parameter = BaseObject::Parameter;

    explicit Node(std::string name, Capability caps = Capability::None) : BaseObject(std::move(name), caps), status_("STDBY") {}

    Interface& addInterface(const std::string& iname, Medium m) {
        interfaces_.push_back(std::make_unique<Interface>(iname, m, this));
        return *interfaces_.back();
    }

    void setStateMachine(std::shared_ptr<state_machine::StateMachine> sm) { stateMachine_ = std::move(sm); }

    std::shared_ptr<state_machine::StateMachine> getStateMachine() const { return stateMachine_; }
    virtual void step(double dt_ms) {
        if (frozen_) return;

        if (!transition_queue.empty()) {
            std::string next = transition_queue.front();
            transition_queue.pop();
            log::debug(name_, "lifecycle {} -> {}", status_, next);
            status_ = next;
        }

        if (stateMachine_) {
            if (!stateMachine_->isActive() && goals_.empty()) {
                stateMachine_->start(this);
            }
            if (stateMachine_->isActive()) {
                stateMachine_->step(this);
            }
        }

        for (auto& child : sub_nodes_) {
            child->step(dt_ms);
        }

        logTelemetry();
    }

    void requestTransition(std::string next) { transition_queue.push(std::move(next)); }

    void addSubNode(std::unique_ptr<Node> node) { sub_nodes_.push_back(std::move(node)); }

    const std::string& status() const { return status_; }
    const std::vector<std::unique_ptr<Node>>& subNodes() const { return sub_nodes_; }
    const std::vector<std::unique_ptr<Interface>>& interfaces() const { return interfaces_; }

    Node* findNode(const std::string& target) {
        if (name_ == target) return this;
        for (auto& child : sub_nodes_) {
            if (auto* hit = child->findNode(target)) return hit;
        }
        return nullptr;
    }

    virtual json::Value describe() const {
        json::Value out = json::Value::object();
        out["name"] = name_;
        out["sid"] = sid_;
        out["status"] = status_;
        out["frozen"] = frozen_;
        out["capabilities"] = static_cast<std::int64_t>(capabilities());
        json::Value params = json::Value::object();
        for (const auto& [key, value] : params_) {
            std::visit([&](const auto& v) { params[key] = v; }, value);
        }
        out["parameters"] = std::move(params);
        json::Value ifaces = json::Value::array();
        for (const auto& i : interfaces_) {
            json::Value item = json::Value::object();
            item["name"] = i->name();
            item["medium"] = std::string(mediumToString(i->medium()));
            item["connected"] = i->isConnected();
            ifaces.push(std::move(item));
        }
        out["interfaces"] = std::move(ifaces);
        json::Value children = json::Value::array();
        for (const auto& child : sub_nodes_) children.push(child->describe());
        out["children"] = std::move(children);
        return out;
    }

protected:
    virtual void logTelemetry() {}

    std::string status_;
    std::shared_ptr<state_machine::StateMachine> stateMachine_;
    std::vector<std::unique_ptr<Interface>> interfaces_;
    std::vector<std::unique_ptr<Node>> sub_nodes_;
    std::queue<std::string> transition_queue;
};

}
