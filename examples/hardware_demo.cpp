#include <iostream>
#include <memory>
#include <string>

#include <gygax/core/json.hpp>
#include <gygax/core/log.hpp>
#include <gygax/core/object.hpp>

import gygax.core.base;
import gygax.core.messaging;
import gygax.hardware.core;
import gygax.hardware.brains;
import gygax.hardware.sensors;
import gygax.hardware.actuators;
import gygax.hardware.connectors;
import gygax.state_machine;

using namespace gygax;
using namespace gygax::hardware;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << "\n";
    if (!ok) ++failures;
}

class Recorder : public BaseObject {
public:
    explicit Recorder(std::string name) : BaseObject(std::move(name), Capability::Communication) {}

    void onMessage(const std::string& sender, const std::string& msg) override { messages.push_back(sender + ": " + msg); }

    void onEvent(const std::string& type, const std::string& sender, const std::string& data) override {
        events.push_back(type + " from " + sender + ": " + data);
    }

    std::vector<std::string> messages;
    std::vector<std::string> events;
};

void capabilities() {
    std::cout << "\n[capabilities] aggregation across a node tree\n";
    auto arm = std::make_unique<RoboticArmNode>();
    check(!hasCapability(arm->capabilities(), Capability::Locomotion), "arm alone cannot move");
    arm->addSubNode(std::make_unique<MotionNode>("Wheels"));
    check(hasCapability(arm->capabilities(), Capability::Locomotion), "attaching wheels grants locomotion to the assembly");
    check(arm->compatible(Capability::Manipulation), "arm keeps manipulation");
    CentralCompute brain("Brain");
    check(!arm->compatible(brain), "arm and brain share no capabilities");
    LiDAR lidar;
    VisualSensor camera;
    check(lidar.compatible(camera), "two perception nodes are compatible");
}

void messaging() {
    std::cout << "\n[messaging] hub delivery, buffering and events\n";
    Recorder controller("controller");
    Recorder actuator("actuator");
    controller.initialize();
    controller.sendMessage("actuator", "queued before the actuator exists");
    actuator.initialize();
    check(actuator.messages.size() == 1, "message sent to an offline node is delivered on registration");
    controller.sendMessage("actuator", "rotate 45");
    check(actuator.messages.back() == "controller: rotate 45", "direct delivery");
    actuator.triggerEvent("SAFETY_ALERT", "obstacle");
    check(controller.events.size() == 1 && actuator.events.empty(), "events reach everyone except the sender");
    controller.pre_delete();
    actuator.pre_delete();
}

void state() {
    std::cout << "\n[state] freezing, thoughts and goals\n";
    CentralCompute brain("Orchestrator");
    brain.setParameter("priority", 1);
    brain.freeze(true);
    brain.setParameter("priority", 99);
    check(std::get<int>(*brain.getParameter("priority")) == 1, "frozen nodes reject parameter changes");
    brain.reset();
    brain.addGoal("maintain thermal equilibrium");
    for (int i = 1; i <= 40; ++i) brain.pushThought("analyzing sensor pack " + std::to_string(i));
    check(brain.thoughts().size() <= DEFAULT_MAX_THOUGHTS, "thought buffer compacts at its limit");
    check(brain.summarizeThoughts().find("maintain thermal equilibrium") != std::string::npos, "summaries carry the goals");
}

void machine() {
    std::cout << "\n[state machine] hierarchical control over node parameters\n";
    auto node = std::make_shared<Node>("Thermostat");
    auto sm = std::make_shared<state_machine::StateMachine>();
    auto idle = std::make_shared<state_machine::State>("Idle");
    auto heating = std::make_shared<state_machine::State>("Heating");
    idle->addTransition(std::make_shared<state_machine::ParameterThresholdCondition>(
                            "temperature", 18.0F, state_machine::ParameterThresholdCondition::Op::LessThan),
                        "Heating");
    heating->addTransition(std::make_shared<state_machine::ParameterThresholdCondition>(
                               "temperature", 21.0F, state_machine::ParameterThresholdCondition::Op::GreaterThan),
                           "Idle");
    sm->addState(idle);
    sm->addState(heating);
    sm->setInitialState("Idle");
    node->setStateMachine(sm);
    node->setParameter("temperature", 20.0F);
    node->step(10.0);
    check(sm->currentState()->name() == "Idle", "stays idle at 20 C");
    node->setParameter("temperature", 15.0F);
    node->step(10.0);
    check(sm->currentState()->name() == "Heating", "switches to heating below 18 C");
    node->setParameter("temperature", 22.0F);
    node->step(10.0);
    check(sm->currentState()->name() == "Idle", "returns to idle above 21 C");
    check(sm->transitionsCount() == 2, "two transitions were recorded");
}

void neuromorphic() {
    std::cout << "\n[neuromorphic] a spiking node inside the same hierarchy\n";
    auto chassis = std::make_unique<Node>("Chassis", Capability::Locomotion);
    auto snn = std::make_unique<NeuromorphicNode>("Cortex", 0.1, 3);
    auto& net = snn->network();
    const auto in = net.addPoisson("in", 2, 0.0);
    const auto out = net.addLif("out", 2);
    net.connectExplicit(in, out, {0, 1}, {0, 1}, {14.0F, 14.0F}, 1.0);
    check(snn->bindInput("in") && snn->bindOutput("out"), "input and output populations bound");
    auto* cortex = snn.get();
    chassis->addSubNode(std::move(snn));
    check(hasCapability(chassis->capabilities(), Capability::Locomotion), "chassis capabilities intact");
    cortex->encode({0.0, 1.0}, 300.0);
    chassis->step(300.0);
    check(cortex->decision() == 1, "the stimulated channel wins the readout");
    const auto description = chassis->describe();
    check(description.find("children")->asArray().size() == 1 && description.find("children")->asArray()[0].contains("network"),
          "node descriptions include the network");
    std::cout << description.dump(2) << "\n";
}

} // namespace

int main() {
    log::setLevel(log::Level::Warn);
    std::cout << "Gygax hardware object model\n";
    capabilities();
    messaging();
    state();
    machine();
    neuromorphic();
    std::cout << "\n" << (failures == 0 ? "HARDWARE DEMO OK" : "HARDWARE DEMO FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
