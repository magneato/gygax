module;
#include <string>

export module gygax.hardware.connectors;
import gygax.hardware.core;

export namespace gygax::hardware {

class TransmissionLink : public Link {
public:
    using Link::Link;
};

class PhysicalCable : public TransmissionLink {
public:
    PhysicalCable(std::string name, Medium m) : TransmissionLink(std::move(name), m) {}
};

class NetworkFabricLink : public TransmissionLink {
public:
    NetworkFabricLink() : TransmissionLink("FiberMesh", Medium::Data_Fiber) {}
};

class IndustrialBus : public TransmissionLink {
public:
    IndustrialBus() : TransmissionLink("LadderLogicRelay", Medium::Data_LadderLogic) {}
};

class FluidicLink : public TransmissionLink {
public:
    FluidicLink(Medium m) : TransmissionLink("ReinforcedHose", m) {}
};

class EntanglementLink : public TransmissionLink {
public:
    EntanglementLink() : TransmissionLink("QuantumChannel", Medium::Quantum_Entanglement) {}
};

}
