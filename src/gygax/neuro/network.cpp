#include <gygax/neuro/network.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace gygax::neuro {

namespace {

constexpr std::size_t kMaxRecordedSpikes = 5'000'000;

std::uint64_t splitmix(std::uint64_t& x) {
    x += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::uint64_t rotl(std::uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

}

Rng::Rng(std::uint64_t seedValue) {
    seed(seedValue);
}

void Rng::seed(std::uint64_t seedValue) {
    std::uint64_t x = seedValue;
    for (auto& word : s_) word = splitmix(x);
    haveSpare_ = false;
}

std::uint64_t Rng::next() {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

double Rng::uniform() {
    return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
}

double Rng::normal() {
    if (haveSpare_) {
        haveSpare_ = false;
        return spare_;
    }
    double u = 0.0;
    double v = 0.0;
    double s = 0.0;
    do {
        u = uniform() * 2.0 - 1.0;
        v = uniform() * 2.0 - 1.0;
        s = u * u + v * v;
    } while (s >= 1.0 || s == 0.0);
    const double factor = std::sqrt(-2.0 * std::log(s) / s);
    spare_ = v * factor;
    haveSpare_ = true;
    return u * factor;
}

bool Rng::bernoulli(double p) {
    return uniform() < p;
}

struct Network::Impl {
    struct Population {
        std::string name;
        Model model = Model::Lif;
        std::size_t n = 0;
        LifParams lif;
        IzhikevichParams izh;
        std::vector<double> v;
        std::vector<double> u;
        std::vector<double> refractory;
        std::vector<double> isyn;
        std::vector<double> bias;
        std::vector<double> rate;
        std::vector<std::uint32_t> counts;
        std::vector<std::uint32_t> pendingInjection;
        std::vector<std::uint32_t> spiked;
        std::vector<Spike> record;
        std::vector<double> vTrace;
        bool recordVoltage = false;
        double synDecay = 0.0;
        double memDecay = 0.0;
        std::size_t refractorySteps = 0;
    };

    struct Projection {
        PopulationId pre = 0;
        PopulationId post = 0;
        std::vector<std::uint32_t> preIdx;
        std::vector<std::uint32_t> postIdx;
        std::vector<float> weight;
        std::vector<float> initialWeight;
        std::vector<std::uint16_t> delaySteps;
        std::vector<std::size_t> outOffset;
        std::vector<std::uint32_t> outList;
        std::vector<std::size_t> inOffset;
        std::vector<std::uint32_t> inList;
        std::optional<StdpParams> stdp;
        std::vector<double> preTrace;
        std::vector<double> postTrace;
        double decayPlus = 0.0;
        double decayMinus = 0.0;
    };

    double dt;
    std::uint64_t seed;
    Rng rng;
    std::vector<Population> pops;
    std::vector<Projection> projs;
    std::vector<std::vector<std::vector<double>>> ring;
    std::size_t ringSlots = 2;
    std::size_t cursor = 0;
    double time = 0.0;
    bool plasticity = true;
    std::uint64_t dropped = 0;

    Impl(double dtMs, std::uint64_t s) : dt(dtMs), seed(s), rng(s) {
        if (!(dtMs > 0.0) || dtMs > 10.0) throw std::invalid_argument("dt must be in (0, 10] ms");
    }

    Population& checked(PopulationId id) {
        if (id >= pops.size()) throw std::out_of_range("unknown population");
        return pops[id];
    }

    const Population& checked(PopulationId id) const {
        if (id >= pops.size()) throw std::out_of_range("unknown population");
        return pops[id];
    }

    Projection& checkedProj(ProjectionId id) {
        if (id >= projs.size()) throw std::out_of_range("unknown projection");
        return projs[id];
    }

    void initState(Population& p) const {
        p.v.assign(p.n, p.model == Model::Izhikevich ? p.izh.c : p.lif.vRest);
        p.u.assign(p.n, p.model == Model::Izhikevich ? p.izh.b * p.izh.c : 0.0);
        p.refractory.assign(p.n, 0.0);
        p.isyn.assign(p.n, 0.0);
        p.counts.assign(p.n, 0);
        p.pendingInjection.clear();
        p.spiked.clear();
        p.record.clear();
        p.vTrace.clear();
        const double tauSyn = p.model == Model::Izhikevich ? p.izh.tauSyn : p.lif.tauSyn;
        p.synDecay = std::exp(-dt / std::max(tauSyn, 1e-6));
        p.memDecay = std::exp(-dt / std::max(p.lif.tauM, 1e-6));
        p.refractorySteps = static_cast<std::size_t>(std::ceil(p.lif.tauRef / dt));
    }

    PopulationId addPop(std::string name, Model model, std::size_t n) {
        if (n == 0) throw std::invalid_argument("population must have at least one neuron");
        if (n > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("population too large");
        for (const auto& existing : pops) {
            if (existing.name == name) throw std::invalid_argument("duplicate population name: " + name);
        }
        Population p;
        p.name = std::move(name);
        p.model = model;
        p.n = n;
        p.bias.assign(n, 0.0);
        p.rate.assign(n, 0.0);
        pops.push_back(std::move(p));
        return pops.size() - 1;
    }

    void ensureRing(std::size_t delaySteps) {
        if (delaySteps + 1 > ringSlots) ringSlots = delaySteps + 1;
        ring.resize(pops.size());
        for (std::size_t i = 0; i < pops.size(); ++i) {
            ring[i].resize(ringSlots);
            for (auto& slot : ring[i]) slot.resize(pops[i].n, 0.0);
        }
    }

    void finalizeProjection(Projection& pr) {
        const std::size_t preN = pops[pr.pre].n;
        const std::size_t postN = pops[pr.post].n;
        const std::size_t count = pr.weight.size();
        pr.outOffset.assign(preN + 1, 0);
        pr.inOffset.assign(postN + 1, 0);
        for (std::size_t s = 0; s < count; ++s) {
            ++pr.outOffset[pr.preIdx[s] + 1];
            ++pr.inOffset[pr.postIdx[s] + 1];
        }
        for (std::size_t i = 0; i < preN; ++i) pr.outOffset[i + 1] += pr.outOffset[i];
        for (std::size_t i = 0; i < postN; ++i) pr.inOffset[i + 1] += pr.inOffset[i];
        pr.outList.assign(count, 0);
        pr.inList.assign(count, 0);
        std::vector<std::size_t> outFill(pr.outOffset.begin(), pr.outOffset.end() - 1);
        std::vector<std::size_t> inFill(pr.inOffset.begin(), pr.inOffset.end() - 1);
        for (std::size_t s = 0; s < count; ++s) {
            pr.outList[outFill[pr.preIdx[s]]++] = static_cast<std::uint32_t>(s);
            pr.inList[inFill[pr.postIdx[s]]++] = static_cast<std::uint32_t>(s);
        }
        pr.initialWeight = pr.weight;
        if (pr.stdp) {
            pr.preTrace.assign(preN, 0.0);
            pr.postTrace.assign(postN, 0.0);
            pr.decayPlus = std::exp(-dt / std::max(pr.stdp->tauPlus, 1e-6));
            pr.decayMinus = std::exp(-dt / std::max(pr.stdp->tauMinus, 1e-6));
        }
    }

    std::uint16_t toSteps(double delayMs) const {
        const double steps = std::max(1.0, std::round(delayMs / dt));
        if (steps > 60000.0) throw std::invalid_argument("delay too long for the configured dt");
        return static_cast<std::uint16_t>(steps);
    }

    void record(Population& p, std::uint32_t idx) {
        ++p.counts[idx];
        p.spiked.push_back(idx);
        if (p.record.size() < kMaxRecordedSpikes)
            p.record.push_back({time, idx});
        else
            ++dropped;
    }

    void updatePopulation(Population& p) {
        p.spiked.clear();
        auto& incoming = ring[static_cast<std::size_t>(&p - pops.data())][cursor];
        switch (p.model) {
        case Model::Poisson:
            for (std::size_t i = 0; i < p.n; ++i) {
                const double prob = p.rate[i] * dt / 1000.0;
                if (prob > 0.0 && rng.uniform() < prob) record(p, static_cast<std::uint32_t>(i));
            }
            break;
        case Model::SpikeSource:
            for (const auto idx : p.pendingInjection) record(p, idx);
            p.pendingInjection.clear();
            break;
        case Model::Lif:
            for (std::size_t i = 0; i < p.n; ++i) {
                p.isyn[i] = p.isyn[i] * p.synDecay + incoming[i];
                incoming[i] = 0.0;
                if (p.refractory[i] > 0.0) {
                    p.refractory[i] -= 1.0;
                    p.v[i] = p.lif.vReset;
                    continue;
                }
                const double vInf = p.lif.vRest + p.lif.resistance * (p.isyn[i] + p.bias[i]);
                p.v[i] = vInf + (p.v[i] - vInf) * p.memDecay;
                if (p.v[i] >= p.lif.vThresh) {
                    p.v[i] = p.lif.vReset;
                    p.refractory[i] = static_cast<double>(p.refractorySteps);
                    record(p, static_cast<std::uint32_t>(i));
                }
            }
            break;
        case Model::Izhikevich: {
            const int sub = std::max(1, static_cast<int>(std::ceil(dt / 0.5)));
            const double h = dt / sub;
            for (std::size_t i = 0; i < p.n; ++i) {
                p.isyn[i] = p.isyn[i] * p.synDecay + incoming[i];
                incoming[i] = 0.0;
                const double current = p.isyn[i] + p.bias[i];
                bool fired = false;
                for (int k = 0; k < sub; ++k) {
                    const double v = p.v[i];
                    p.v[i] += h * (0.04 * v * v + 5.0 * v + 140.0 - p.u[i] + current);
                    p.u[i] += h * p.izh.a * (p.izh.b * p.v[i] - p.u[i]);
                    if (p.v[i] >= 30.0) {
                        p.v[i] = p.izh.c;
                        p.u[i] += p.izh.d;
                        fired = true;
                    }
                }
                if (fired) record(p, static_cast<std::uint32_t>(i));
            }
            break;
        }
        }
        if (p.recordVoltage && p.model != Model::Poisson && p.model != Model::SpikeSource && !p.v.empty()) p.vTrace.push_back(p.v[0]);
    }

    void propagate() {
        for (auto& pr : projs) {
            const auto& pre = pops[pr.pre];
            const auto& post = pops[pr.post];
            auto& slots = ring[pr.post];
            if (pr.stdp) {
                for (auto& t : pr.preTrace) t *= pr.decayPlus;
                for (auto& t : pr.postTrace) t *= pr.decayMinus;
            }
            for (const auto idx : pre.spiked) {
                for (std::size_t o = pr.outOffset[idx]; o < pr.outOffset[idx + 1]; ++o) {
                    const std::uint32_t s = pr.outList[o];
                    const std::size_t slot = (cursor + pr.delaySteps[s]) % ringSlots;
                    slots[slot][pr.postIdx[s]] += static_cast<double>(pr.weight[s]);
                    if (pr.stdp && plasticity) {
                        const double w = static_cast<double>(pr.weight[s]) - pr.stdp->aMinus * pr.postTrace[pr.postIdx[s]];
                        pr.weight[s] = static_cast<float>(std::clamp(w, pr.stdp->wMin, pr.stdp->wMax));
                    }
                }
                if (pr.stdp) pr.preTrace[idx] += 1.0;
            }
            if (pr.stdp) {
                for (const auto idx : post.spiked) {
                    for (std::size_t o = pr.inOffset[idx]; o < pr.inOffset[idx + 1]; ++o) {
                        const std::uint32_t s = pr.inList[o];
                        if (plasticity) {
                            const double w = static_cast<double>(pr.weight[s]) + pr.stdp->aPlus * pr.preTrace[pr.preIdx[s]];
                            pr.weight[s] = static_cast<float>(std::clamp(w, pr.stdp->wMin, pr.stdp->wMax));
                        }
                    }
                    pr.postTrace[idx] += 1.0;
                }
            }
        }
    }

    void step() {
        if (ring.size() != pops.size() || ring.empty()) ensureRing(ringSlots);
        for (auto& p : pops) updatePopulation(p);
        propagate();
        cursor = (cursor + 1) % ringSlots;
        time += dt;
    }
};

Network::Network(double dtMs, std::uint64_t seed) : impl_(std::make_unique<Impl>(dtMs, seed)) {}
Network::~Network() = default;
Network::Network(Network&&) noexcept = default;
Network& Network::operator=(Network&&) noexcept = default;

PopulationId Network::addLif(std::string name, std::size_t count, const LifParams& params) {
    if (params.tauM <= 0.0 || params.vThresh <= params.vReset) throw std::invalid_argument("invalid LIF parameters");
    const auto id = impl_->addPop(std::move(name), Model::Lif, count);
    impl_->pops[id].lif = params;
    impl_->initState(impl_->pops[id]);
    impl_->ensureRing(impl_->ringSlots);
    return id;
}

PopulationId Network::addIzhikevich(std::string name, std::size_t count, const IzhikevichParams& params) {
    const auto id = impl_->addPop(std::move(name), Model::Izhikevich, count);
    impl_->pops[id].izh = params;
    impl_->initState(impl_->pops[id]);
    impl_->ensureRing(impl_->ringSlots);
    return id;
}

PopulationId Network::addPoisson(std::string name, std::size_t count, double rateHz) {
    if (rateHz < 0.0) throw std::invalid_argument("rate must be non-negative");
    const auto id = impl_->addPop(std::move(name), Model::Poisson, count);
    impl_->pops[id].rate.assign(count, rateHz);
    impl_->initState(impl_->pops[id]);
    impl_->ensureRing(impl_->ringSlots);
    return id;
}

PopulationId Network::addSpikeSource(std::string name, std::size_t count) {
    const auto id = impl_->addPop(std::move(name), Model::SpikeSource, count);
    impl_->initState(impl_->pops[id]);
    impl_->ensureRing(impl_->ringSlots);
    return id;
}

ProjectionId Network::connect(PopulationId pre, PopulationId post, const ConnectionSpec& spec) {
    auto& preP = impl_->checked(pre);
    auto& postP = impl_->checked(post);
    if (postP.model == Model::Poisson || postP.model == Model::SpikeSource)
        throw std::invalid_argument("cannot project onto an input population");
    if (spec.kind == Connectivity::OneToOne && preP.n != postP.n) throw std::invalid_argument("one-to-one needs equal population sizes");
    if (spec.kind == Connectivity::FixedProbability && (spec.probability < 0.0 || spec.probability > 1.0)) {
        throw std::invalid_argument("probability must be within [0, 1]");
    }
    if (spec.stdp && spec.stdp->wMax < spec.stdp->wMin) throw std::invalid_argument("invalid STDP bounds");

    Impl::Projection pr;
    pr.pre = pre;
    pr.post = post;
    pr.stdp = spec.stdp;
    const auto steps = impl_->toSteps(spec.delayMs);
    auto add = [&](std::size_t i, std::size_t j) {
        double w = spec.weightMean;
        if (spec.weightStd > 0.0) {
            w += spec.weightStd * impl_->rng.normal();
            if (spec.weightMean >= 0.0)
                w = std::max(0.0, w);
            else
                w = std::min(0.0, w);
        }
        pr.preIdx.push_back(static_cast<std::uint32_t>(i));
        pr.postIdx.push_back(static_cast<std::uint32_t>(j));
        pr.weight.push_back(static_cast<float>(w));
        pr.delaySteps.push_back(steps);
    };
    switch (spec.kind) {
    case Connectivity::AllToAll:
        for (std::size_t i = 0; i < preP.n; ++i)
            for (std::size_t j = 0; j < postP.n; ++j)
                if (pre != post || spec.allowSelf || i != j) add(i, j);
        break;
    case Connectivity::OneToOne:
        for (std::size_t i = 0; i < preP.n; ++i) add(i, i);
        break;
    case Connectivity::FixedProbability:
        for (std::size_t i = 0; i < preP.n; ++i)
            for (std::size_t j = 0; j < postP.n; ++j)
                if ((pre != post || spec.allowSelf || i != j) && impl_->rng.bernoulli(spec.probability)) add(i, j);
        break;
    }
    impl_->ensureRing(steps);
    impl_->finalizeProjection(pr);
    impl_->projs.push_back(std::move(pr));
    return impl_->projs.size() - 1;
}

ProjectionId Network::connectExplicit(PopulationId pre, PopulationId post, const std::vector<std::uint32_t>& preIdx,
                                      const std::vector<std::uint32_t>& postIdx, const std::vector<float>& weights, double delayMs,
                                      std::optional<StdpParams> stdp) {
    auto& preP = impl_->checked(pre);
    auto& postP = impl_->checked(post);
    if (preIdx.size() != postIdx.size() || preIdx.size() != weights.size())
        throw std::invalid_argument("explicit connection arrays differ in length");
    if (postP.model == Model::Poisson || postP.model == Model::SpikeSource)
        throw std::invalid_argument("cannot project onto an input population");
    for (std::size_t s = 0; s < preIdx.size(); ++s) {
        if (preIdx[s] >= preP.n || postIdx[s] >= postP.n) throw std::out_of_range("synapse index out of range");
    }
    Impl::Projection pr;
    pr.pre = pre;
    pr.post = post;
    pr.stdp = stdp;
    pr.preIdx = preIdx;
    pr.postIdx = postIdx;
    pr.weight = weights;
    const auto steps = impl_->toSteps(delayMs);
    pr.delaySteps.assign(weights.size(), steps);
    impl_->ensureRing(steps);
    impl_->finalizeProjection(pr);
    impl_->projs.push_back(std::move(pr));
    return impl_->projs.size() - 1;
}

void Network::setBias(PopulationId pop, double current) {
    auto& p = impl_->checked(pop);
    p.bias.assign(p.n, current);
}

void Network::setDrive(PopulationId pop, const std::vector<double>& currents) {
    auto& p = impl_->checked(pop);
    if (currents.size() != p.n) throw std::invalid_argument("drive vector length must equal the population size");
    p.bias = currents;
}

void Network::setRate(PopulationId pop, double rateHz) {
    auto& p = impl_->checked(pop);
    if (p.model != Model::Poisson) throw std::invalid_argument("population is not Poisson");
    if (rateHz < 0.0) throw std::invalid_argument("rate must be non-negative");
    p.rate.assign(p.n, rateHz);
}

void Network::setRates(PopulationId pop, const std::vector<double>& ratesHz) {
    auto& p = impl_->checked(pop);
    if (p.model != Model::Poisson) throw std::invalid_argument("population is not Poisson");
    if (ratesHz.size() != p.n) throw std::invalid_argument("rate vector length must equal the population size");
    for (const double r : ratesHz) {
        if (r < 0.0) throw std::invalid_argument("rate must be non-negative");
    }
    p.rate = ratesHz;
}

void Network::injectSpike(PopulationId pop, std::size_t neuron) {
    auto& p = impl_->checked(pop);
    if (p.model != Model::SpikeSource) throw std::invalid_argument("population is not a spike source");
    if (neuron >= p.n) throw std::out_of_range("neuron index out of range");
    p.pendingInjection.push_back(static_cast<std::uint32_t>(neuron));
}

void Network::setRecordVoltage(PopulationId pop, bool enabled) {
    impl_->checked(pop).recordVoltage = enabled;
}
void Network::setPlasticity(bool enabled) {
    impl_->plasticity = enabled;
}

void Network::step() {
    impl_->step();
}

void Network::run(double durationMs) {
    if (durationMs < 0.0) throw std::invalid_argument("duration must be non-negative");
    const auto steps = static_cast<std::size_t>(std::llround(durationMs / impl_->dt));
    for (std::size_t i = 0; i < steps; ++i) impl_->step();
}

void Network::reset(bool restoreWeights) {
    impl_->rng.seed(impl_->seed);
    impl_->time = 0.0;
    impl_->cursor = 0;
    impl_->dropped = 0;
    for (auto& slots : impl_->ring) {
        for (auto& slot : slots) std::ranges::fill(slot, 0.0);
    }
    for (auto& p : impl_->pops) impl_->initState(p);
    for (auto& pr : impl_->projs) {
        if (restoreWeights) pr.weight = pr.initialWeight;
        if (pr.stdp) {
            std::ranges::fill(pr.preTrace, 0.0);
            std::ranges::fill(pr.postTrace, 0.0);
        }
    }
}

double Network::dtMs() const {
    return impl_->dt;
}
double Network::timeMs() const {
    return impl_->time;
}
std::size_t Network::populationCount() const {
    return impl_->pops.size();
}
std::size_t Network::projectionCount() const {
    return impl_->projs.size();
}

std::optional<PopulationId> Network::findPopulation(std::string_view name) const {
    for (std::size_t i = 0; i < impl_->pops.size(); ++i) {
        if (impl_->pops[i].name == name) return i;
    }
    return std::nullopt;
}

const std::string& Network::populationName(PopulationId pop) const {
    return impl_->checked(pop).name;
}
std::size_t Network::populationSize(PopulationId pop) const {
    return impl_->checked(pop).n;
}
Model Network::populationModel(PopulationId pop) const {
    return impl_->checked(pop).model;
}
const std::vector<Spike>& Network::spikes(PopulationId pop) const {
    return impl_->checked(pop).record;
}

std::uint64_t Network::spikeCount(PopulationId pop) const {
    const auto& c = impl_->checked(pop).counts;
    return std::accumulate(c.begin(), c.end(), std::uint64_t{0});
}

std::vector<std::uint32_t> Network::spikeCounts(PopulationId pop) const {
    return impl_->checked(pop).counts;
}

double Network::meanRateHz(PopulationId pop) const {
    if (impl_->time <= 0.0) return 0.0;
    return static_cast<double>(spikeCount(pop)) / static_cast<double>(impl_->checked(pop).n) / (impl_->time / 1000.0);
}

std::vector<double> Network::membranePotentials(PopulationId pop) const {
    return impl_->checked(pop).v;
}
const std::vector<double>& Network::voltageTrace(PopulationId pop) const {
    return impl_->checked(pop).vTrace;
}

std::vector<float> Network::weights(ProjectionId proj) const {
    if (proj >= impl_->projs.size()) throw std::out_of_range("unknown projection");
    return impl_->projs[proj].weight;
}

double Network::meanWeight(ProjectionId proj) const {
    const auto w = weights(proj);
    if (w.empty()) return 0.0;
    return std::accumulate(w.begin(), w.end(), 0.0) / static_cast<double>(w.size());
}

std::size_t Network::synapseCount(ProjectionId proj) const {
    if (proj >= impl_->projs.size()) throw std::out_of_range("unknown projection");
    return impl_->projs[proj].weight.size();
}

std::uint64_t Network::droppedSpikes() const {
    return impl_->dropped;
}

void Network::clearSpikes() {
    for (auto& p : impl_->pops) {
        p.record.clear();
        std::ranges::fill(p.counts, 0U);
    }
    impl_->dropped = 0;
}

json::Value Network::describe() const {
    json::Value out = json::Value::object();
    out["dt_ms"] = impl_->dt;
    out["time_ms"] = impl_->time;
    json::Value pops = json::Value::array();
    for (const auto& p : impl_->pops) {
        json::Value item = json::Value::object();
        item["name"] = p.name;
        static constexpr const char* names[] = {"lif", "izhikevich", "poisson", "spike_source"};
        item["type"] = names[static_cast<int>(p.model)];
        item["size"] = p.n;
        item["spikes"] = std::accumulate(p.counts.begin(), p.counts.end(), std::uint64_t{0});
        pops.push(std::move(item));
    }
    out["populations"] = std::move(pops);
    json::Value projs = json::Value::array();
    for (std::size_t i = 0; i < impl_->projs.size(); ++i) {
        const auto& pr = impl_->projs[i];
        json::Value item = json::Value::object();
        item["pre"] = impl_->pops[pr.pre].name;
        item["post"] = impl_->pops[pr.post].name;
        item["synapses"] = pr.weight.size();
        item["mean_weight"] = meanWeight(i);
        item["plastic"] = pr.stdp.has_value();
        projs.push(std::move(item));
    }
    out["projections"] = std::move(projs);
    return out;
}

std::vector<double> rateEncode(const std::vector<double>& values, double maxRateHz) {
    std::vector<double> rates;
    rates.reserve(values.size());
    for (const double v : values) rates.push_back(std::clamp(v, 0.0, 1.0) * maxRateHz);
    return rates;
}

std::size_t argmaxCounts(const std::vector<std::uint32_t>& counts) {
    if (counts.empty()) return 0;
    return static_cast<std::size_t>(std::distance(counts.begin(), std::ranges::max_element(counts)));
}

double lifSteadyStateRateHz(const LifParams& params, double currentNa) {
    const double vInf = params.vRest + params.resistance * currentNa;
    if (vInf <= params.vThresh) return 0.0;
    const double t = params.tauM * std::log((vInf - params.vReset) / (vInf - params.vThresh));
    return 1000.0 / (params.tauRef + t);
}

namespace {

double num(const json::Value& obj, std::string_view key, double fallback) {
    return obj.getDouble(key, fallback);
}

LifParams lifFrom(const json::Value* p) {
    LifParams out;
    if (p == nullptr) return out;
    out.tauM = num(*p, "tau_m", out.tauM);
    out.vRest = num(*p, "v_rest", out.vRest);
    out.vReset = num(*p, "v_reset", out.vReset);
    out.vThresh = num(*p, "v_thresh", out.vThresh);
    out.tauRef = num(*p, "tau_ref", out.tauRef);
    out.tauSyn = num(*p, "tau_syn", out.tauSyn);
    out.resistance = num(*p, "resistance", out.resistance);
    return out;
}

IzhikevichParams izhFrom(const json::Value* p) {
    IzhikevichParams out;
    if (p == nullptr) return out;
    out.a = num(*p, "a", out.a);
    out.b = num(*p, "b", out.b);
    out.c = num(*p, "c", out.c);
    out.d = num(*p, "d", out.d);
    out.tauSyn = num(*p, "tau_syn", out.tauSyn);
    return out;
}

std::optional<StdpParams> stdpFrom(const json::Value* p) {
    if (p == nullptr || !p->isObject()) return std::nullopt;
    StdpParams out;
    out.aPlus = num(*p, "a_plus", out.aPlus);
    out.aMinus = num(*p, "a_minus", out.aMinus);
    out.tauPlus = num(*p, "tau_plus", out.tauPlus);
    out.tauMinus = num(*p, "tau_minus", out.tauMinus);
    out.wMin = num(*p, "w_min", out.wMin);
    out.wMax = num(*p, "w_max", out.wMax);
    return out;
}

}

std::unique_ptr<Network> Network::fromJson(const json::Value& spec, std::string* error) {
    auto fail = [&](std::string message) -> std::unique_ptr<Network> {
        if (error != nullptr) *error = std::move(message);
        return nullptr;
    };
    if (!spec.isObject()) return fail("network spec must be a JSON object");
    try {
        auto net = std::make_unique<Network>(spec.getDouble("dt", 0.1), static_cast<std::uint64_t>(spec.getInt("seed", 1)));
        const auto* pops = spec.find("populations");
        if (pops == nullptr || !pops->isArray() || pops->asArray().empty()) return fail("spec needs a non-empty 'populations' array");
        if (pops->asArray().size() > 64) return fail("too many populations (limit 64)");
        std::size_t totalNeurons = 0;
        for (const auto& item : pops->asArray()) {
            const auto name = item.getString("name");
            const auto type = item.getString("type", "lif");
            const auto count = static_cast<std::size_t>(std::max<std::int64_t>(0, item.getInt("n", 0)));
            if (name.empty()) return fail("population needs a 'name'");
            totalNeurons += count;
            if (totalNeurons > 200000) return fail("network too large for a spec run (limit 200000 neurons)");
            PopulationId id = 0;
            if (type == "lif")
                id = net->addLif(name, count, lifFrom(item.find("params")));
            else if (type == "izhikevich")
                id = net->addIzhikevich(name, count, izhFrom(item.find("params")));
            else if (type == "poisson")
                id = net->addPoisson(name, count, item.getDouble("rate", 0.0));
            else if (type == "spike_source")
                id = net->addSpikeSource(name, count);
            else
                return fail("unknown population type '" + type + "'");
            if (item.contains("bias")) net->setBias(id, item.getDouble("bias"));
            if (item.getBool("record_voltage", false)) net->setRecordVoltage(id, true);
        }
        if (const auto* projs = spec.find("projections")) {
            for (const auto& item : projs->asArray()) {
                const auto pre = net->findPopulation(item.getString("pre"));
                const auto post = net->findPopulation(item.getString("post"));
                if (!pre || !post) return fail("projection references an unknown population");
                ConnectionSpec cs;
                const auto* conn = item.find("connect");
                const auto kind = conn != nullptr ? conn->getString("type", "fixed_prob") : std::string("fixed_prob");
                if (kind == "all_to_all")
                    cs.kind = Connectivity::AllToAll;
                else if (kind == "one_to_one")
                    cs.kind = Connectivity::OneToOne;
                else if (kind == "fixed_prob")
                    cs.kind = Connectivity::FixedProbability;
                else
                    return fail("unknown connect type '" + kind + "'");
                if (conn != nullptr) {
                    cs.probability = conn->getDouble("p", cs.probability);
                    cs.allowSelf = conn->getBool("allow_self", false);
                }
                if (const auto* w = item.find("weight")) {
                    if (w->isNumber())
                        cs.weightMean = w->asDouble();
                    else {
                        cs.weightMean = w->getDouble("mean", cs.weightMean);
                        cs.weightStd = w->getDouble("std", 0.0);
                    }
                }
                cs.delayMs = item.getDouble("delay_ms", cs.delayMs);
                cs.stdp = stdpFrom(item.find("stdp"));
                net->connect(*pre, *post, cs);
            }
        }
        return net;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

json::Value Network::runSpec(const json::Value& spec, std::string* error) {
    try {
        const double runMs = spec.getDouble("run_ms", 0.0);
        if (runMs < 0.0 || runMs > 600000.0) {
            if (error != nullptr) *error = "run_ms must be within [0, 600000]";
            return {};
        }
        const std::size_t rasterLimit = static_cast<std::size_t>(std::max<std::int64_t>(0, spec.getInt("raster_limit", 0)));
        run(runMs);
        json::Value out = describe();
        json::Value results = json::Value::array();
        for (std::size_t i = 0; i < populationCount(); ++i) {
            json::Value item = json::Value::object();
            item["name"] = populationName(i);
            item["size"] = populationSize(i);
            item["spike_count"] = spikeCount(i);
            item["mean_rate_hz"] = meanRateHz(i);
            if (rasterLimit > 0) {
                json::Value raster = json::Value::array();
                std::size_t taken = 0;
                for (const auto& s : spikes(i)) {
                    if (taken++ >= rasterLimit) break;
                    json::Value pair = json::Value::array();
                    pair.push(s.timeMs);
                    pair.push(static_cast<std::int64_t>(s.neuron));
                    raster.push(std::move(pair));
                }
                item["raster"] = std::move(raster);
            }
            results.push(std::move(item));
        }
        out["results"] = std::move(results);
        out["dropped_spikes"] = droppedSpikes();
        return out;
    } catch (const std::exception& e) {
        if (error != nullptr) *error = e.what();
        return {};
    }
}

json::Value simulateSpec(const json::Value& spec, std::string* error) {
    auto net = Network::fromJson(spec, error);
    if (!net) return {};
    return net->runSpec(spec, error);
}

}
