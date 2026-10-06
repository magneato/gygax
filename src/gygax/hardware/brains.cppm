module;
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <gygax/core/json.hpp>
#include <gygax/neuro/network.hpp>

export module gygax.hardware.brains;

import gygax.hardware.core;
import gygax.core.base;

export namespace gygax::hardware {

class IntelligenceNode : public Node {
public:
    IntelligenceNode(std::string name, Capability extra_caps = Capability::None)
        : Node(std::move(name), Capability::Cognition | extra_caps) {}

    void step(double dt) override { Node::step(dt); }
};

class CentralCompute : public IntelligenceNode {
public:
    CentralCompute(std::string name = "CPU_Node") : IntelligenceNode(std::move(name)) {
        addInterface("SystemBus", Medium::Data_Ethernet);
        addInterface("PowerRail", Medium::Electrical);
        setParameter("architecture", "x86_64_advanced");
    }
};

class AccelerationNode : public IntelligenceNode {
public:
    AccelerationNode(std::string name = "GPU_Accelerator") : IntelligenceNode(std::move(name)) {
        addInterface("PCIe_Gen6", Medium::Data_Ethernet);
        setParameter("tflops", 2000.5f);
    }
};

class QuantumNode : public IntelligenceNode {
public:
    QuantumNode(std::string name = "QuantumCore") : IntelligenceNode(std::move(name), Capability::QuantumCompute) {
        addInterface("CoherenceLink", Medium::Quantum_Entanglement);
        setParameter("qubits", 4096);
    }
};

class NeuromorphicNode : public IntelligenceNode {
public:
    explicit NeuromorphicNode(std::string name = "NeuromorphicCore", double dtMs = 0.1, std::uint64_t seed = 1)
        : IntelligenceNode(std::move(name), Capability::Neuromorphic), network_(std::make_unique<neuro::Network>(dtMs, seed)) {
        addInterface("SpikeIn", Medium::Electrical);
        addInterface("SpikeOut", Medium::Electrical);
        setParameter("dt_ms", static_cast<float>(dtMs));
        setParameter("spikes_total", 0);
    }

    neuro::Network& network() { return *network_; }
    const neuro::Network& network() const { return *network_; }

    bool configure(const json::Value& spec, std::string* error) {
        auto built = neuro::Network::fromJson(spec, error);
        if (!built) return false;
        network_ = std::move(built);
        inputPopulation_.reset();
        outputPopulation_.reset();
        return true;
    }

    bool bindInput(const std::string& population) {
        inputPopulation_ = network_->findPopulation(population);
        return inputPopulation_.has_value() && network_->populationModel(*inputPopulation_) == neuro::Model::Poisson;
    }

    bool bindOutput(const std::string& population) {
        outputPopulation_ = network_->findPopulation(population);
        return outputPopulation_.has_value();
    }

    void encode(const std::vector<double>& normalized, double maxRateHz = 100.0) {
        if (!inputPopulation_) return;
        network_->setRates(*inputPopulation_, neuro::rateEncode(normalized, maxRateHz));
    }

    std::vector<std::uint32_t> readout() const {
        if (!outputPopulation_) return {};
        return network_->spikeCounts(*outputPopulation_);
    }

    std::size_t decision() const { return neuro::argmaxCounts(readout()); }

    void clearOutput() { network_->clearSpikes(); }

    void step(double dt) override {
        if (!frozen_) {
            network_->run(dt);
            std::uint64_t total = 0;
            for (std::size_t i = 0; i < network_->populationCount(); ++i) total += network_->spikeCount(i);
            setParameter("spikes_total", static_cast<int>(std::min<std::uint64_t>(total, 2'000'000'000ULL)));
        }
        IntelligenceNode::step(dt);
    }

    json::Value describe() const override {
        json::Value out = Node::describe();
        out["network"] = network_->describe();
        return out;
    }

private:
    std::unique_ptr<neuro::Network> network_;
    std::optional<neuro::PopulationId> inputPopulation_;
    std::optional<neuro::PopulationId> outputPopulation_;
};

}
