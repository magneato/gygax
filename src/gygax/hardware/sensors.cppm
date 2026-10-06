module;
#include <string>
#include <memory>
#include <vector>

export module gygax.hardware.sensors;
import gygax.hardware.core;
import gygax.core.base;

export namespace gygax::hardware {

class PerceptionNode : public Node {
public:
    PerceptionNode(std::string name, Capability extra_caps = Capability::None)
        : Node(std::move(name), Capability::Perception | extra_caps) {}
};

class VisualSensor : public PerceptionNode {
public:
    VisualSensor() : PerceptionNode("Camera_RGBD", Capability::Communication) { addInterface("StreamOut", Medium::Data_Ethernet); }
};

class LiDAR : public PerceptionNode {
public:
    LiDAR() : PerceptionNode("LiDAR_Scan") {
        addInterface("Pointcloud", Medium::Data_Ethernet);
        setParameter("range_m", 300.0f);
    }
};

class InertialNode : public PerceptionNode {
public:
    InertialNode() : PerceptionNode("IMU_6DOF") { addInterface("I2C_Bus", Medium::Electrical); }
};

}
