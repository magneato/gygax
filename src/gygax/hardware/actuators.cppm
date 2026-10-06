module;
#include <string>
#include <memory>
#include <vector>

export module gygax.hardware.actuators;
import gygax.hardware.core;
import gygax.core.base;

export namespace gygax::hardware {

class ActionNode : public Node {
public:
    ActionNode(std::string name, Capability extra_caps = Capability::None) : Node(std::move(name), Capability::Actuation | extra_caps) {}

    Capability capabilities() const override {
        Capability c = caps_;
        for (const auto& child : sub_nodes_) {
            c = c | child->capabilities();
        }
        return c;
    }
};

class MotionNode : public ActionNode {
public:
    MotionNode(std::string name, Capability extra = Capability::None) : ActionNode(std::move(name), Capability::Locomotion | extra) {}
};

class RoboticArmNode : public ActionNode {
public:
    RoboticArmNode() : ActionNode("IndustrialArm", Capability::Manipulation) {
        addInterface("HighPower", Medium::Electrical);
        setParameter("dof", 6);
    }
};

class HandNode : public ActionNode {
public:
    HandNode() : ActionNode("BionicHand", Capability::Manipulation) { addInterface("Serial", Medium::Electrical); }
};

class FluidicNode : public ActionNode {
public:
    FluidicNode() : ActionNode("PneumaticSystem", Capability::PneumaticControl) { addInterface("AirPressure", Medium::Pneumatic); }
};

}
