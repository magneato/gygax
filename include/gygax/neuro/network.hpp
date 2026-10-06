#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gygax/core/json.hpp>

namespace gygax::neuro {

class Rng {
public:
    explicit Rng(std::uint64_t seed = 1);
    void seed(std::uint64_t seed);
    std::uint64_t next();
    double uniform();
    double normal();
    bool bernoulli(double p);

private:
    std::uint64_t s_[4]{};
    bool haveSpare_ = false;
    double spare_ = 0.0;
};

struct LifParams {
    double tauM = 20.0;
    double vRest = -65.0;
    double vReset = -65.0;
    double vThresh = -50.0;
    double tauRef = 2.0;
    double tauSyn = 5.0;
    double resistance = 1.0;
};

struct IzhikevichParams {
    double a = 0.02;
    double b = 0.2;
    double c = -65.0;
    double d = 8.0;
    double tauSyn = 5.0;
};

struct StdpParams {
    double aPlus = 0.01;
    double aMinus = 0.0105;
    double tauPlus = 20.0;
    double tauMinus = 20.0;
    double wMin = 0.0;
    double wMax = 1.0;
};

enum class Connectivity { AllToAll, OneToOne, FixedProbability };

struct ConnectionSpec {
    Connectivity kind = Connectivity::FixedProbability;
    double probability = 0.1;
    double weightMean = 1.0;
    double weightStd = 0.0;
    double delayMs = 1.0;
    bool allowSelf = false;
    std::optional<StdpParams> stdp;
};

struct alignas(16) Spike {
    double timeMs;
    std::uint32_t neuron;
};

using PopulationId = std::size_t;
using ProjectionId = std::size_t;

enum class Model { Lif, Izhikevich, Poisson, SpikeSource };

class Network {
public:
    explicit Network(double dtMs = 0.1, std::uint64_t seed = 1);
    ~Network();
    Network(Network&&) noexcept;
    Network& operator=(Network&&) noexcept;
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    PopulationId addLif(std::string name, std::size_t count, const LifParams& params = {});
    PopulationId addIzhikevich(std::string name, std::size_t count, const IzhikevichParams& params = {});
    PopulationId addPoisson(std::string name, std::size_t count, double rateHz);
    PopulationId addSpikeSource(std::string name, std::size_t count);

    ProjectionId connect(PopulationId pre, PopulationId post, const ConnectionSpec& spec);
    ProjectionId connectExplicit(PopulationId pre, PopulationId post, const std::vector<std::uint32_t>& preIdx,
                                 const std::vector<std::uint32_t>& postIdx, const std::vector<float>& weights, double delayMs,
                                 std::optional<StdpParams> stdp = std::nullopt);

    void setBias(PopulationId pop, double current);
    void setDrive(PopulationId pop, const std::vector<double>& currents);
    void setRate(PopulationId pop, double rateHz);
    void setRates(PopulationId pop, const std::vector<double>& ratesHz);
    void injectSpike(PopulationId pop, std::size_t neuron);
    void setRecordVoltage(PopulationId pop, bool enabled);
    void setPlasticity(bool enabled);

    void step();
    void run(double durationMs);
    void reset(bool restoreWeights = true);

    [[nodiscard]] double dtMs() const;
    [[nodiscard]] double timeMs() const;
    [[nodiscard]] std::size_t populationCount() const;
    [[nodiscard]] std::size_t projectionCount() const;
    [[nodiscard]] std::optional<PopulationId> findPopulation(std::string_view name) const;
    [[nodiscard]] const std::string& populationName(PopulationId pop) const;
    [[nodiscard]] std::size_t populationSize(PopulationId pop) const;
    [[nodiscard]] Model populationModel(PopulationId pop) const;
    [[nodiscard]] const std::vector<Spike>& spikes(PopulationId pop) const;
    [[nodiscard]] std::uint64_t spikeCount(PopulationId pop) const;
    [[nodiscard]] std::vector<std::uint32_t> spikeCounts(PopulationId pop) const;
    [[nodiscard]] double meanRateHz(PopulationId pop) const;
    [[nodiscard]] std::vector<double> membranePotentials(PopulationId pop) const;
    [[nodiscard]] const std::vector<double>& voltageTrace(PopulationId pop) const;
    [[nodiscard]] std::vector<float> weights(ProjectionId proj) const;
    [[nodiscard]] double meanWeight(ProjectionId proj) const;
    [[nodiscard]] std::size_t synapseCount(ProjectionId proj) const;
    [[nodiscard]] std::uint64_t droppedSpikes() const;
    void clearSpikes();

    [[nodiscard]] json::Value describe() const;

    static std::unique_ptr<Network> fromJson(const json::Value& spec, std::string* error);
    json::Value runSpec(const json::Value& spec, std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::vector<double> rateEncode(const std::vector<double>& values, double maxRateHz);
std::size_t argmaxCounts(const std::vector<std::uint32_t>& counts);
double lifSteadyStateRateHz(const LifParams& params, double currentNa);

json::Value simulateSpec(const json::Value& spec, std::string* error);

}
