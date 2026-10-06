#include <gygax/capi/gygax.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include <gygax/core/json.hpp>
#include <gygax/neuro/network.hpp>
#include <gygax/sim/vec_env.hpp>
#include <gygax/version.hpp>

struct gygax_network {
    std::unique_ptr<gygax::neuro::Network> net;
};

struct gygax_vecenv {
    std::unique_ptr<gygax::sim::VecEnv> env;
};

namespace {

thread_local std::string gLastError;

void setError(const std::string& message) {
    gLastError = message;
}

template <typename Fn> int guarded(Fn&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        setError(e.what());
        return -1;
    }
}

template <typename Fn> long long guardedLong(Fn&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        setError(e.what());
        return -1;
    }
}

std::optional<gygax::neuro::StdpParams> stdpFrom(const double* p) {
    if (p == nullptr) return std::nullopt;
    gygax::neuro::StdpParams s;
    s.aPlus = p[0];
    s.aMinus = p[1];
    s.tauPlus = p[2];
    s.tauMinus = p[3];
    s.wMin = p[4];
    s.wMax = p[5];
    return s;
}

char* duplicate(const std::string& text) {
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) {
        setError("out of memory");
        return nullptr;
    }
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

template <typename T> long long copyOut(const std::vector<T>& src, T* dst, size_t capacity) {
    if (dst != nullptr) std::copy_n(src.begin(), std::min(capacity, src.size()), dst);
    return static_cast<long long>(src.size());
}

}

extern "C" {

const char* gygax_version(void) {
    return GYGAX_VERSION_STRING;
}
const char* gygax_last_error(void) {
    return gLastError.c_str();
}
void gygax_free(void* ptr) {
    std::free(ptr);
}

gygax_network* gygax_network_create(double dt_ms, uint64_t seed) {
    try {
        auto* h = new gygax_network{std::make_unique<gygax::neuro::Network>(dt_ms, seed)};
        return h;
    } catch (const std::exception& e) {
        setError(e.what());
        return nullptr;
    }
}

gygax_network* gygax_network_from_json(const char* spec_json) {
    if (spec_json == nullptr) {
        setError("spec is null");
        return nullptr;
    }
    std::string error;
    auto spec = gygax::json::parse(spec_json, &error);
    if (!spec) {
        setError("invalid JSON: " + error);
        return nullptr;
    }
    auto net = gygax::neuro::Network::fromJson(*spec, &error);
    if (!net) {
        setError(error);
        return nullptr;
    }
    return new gygax_network{std::move(net)};
}

void gygax_network_destroy(gygax_network* net) {
    delete net;
}

int gygax_network_add_lif(gygax_network* net, const char* name, size_t count, const double* p) {
    return guarded([&] {
        gygax::neuro::LifParams lp;
        if (p != nullptr) {
            lp.tauM = p[0];
            lp.vRest = p[1];
            lp.vReset = p[2];
            lp.vThresh = p[3];
            lp.tauRef = p[4];
            lp.tauSyn = p[5];
            lp.resistance = p[6];
        }
        return static_cast<int>(net->net->addLif(name, count, lp));
    });
}

int gygax_network_add_izhikevich(gygax_network* net, const char* name, size_t count, const double* p) {
    return guarded([&] {
        gygax::neuro::IzhikevichParams ip;
        if (p != nullptr) {
            ip.a = p[0];
            ip.b = p[1];
            ip.c = p[2];
            ip.d = p[3];
            ip.tauSyn = p[4];
        }
        return static_cast<int>(net->net->addIzhikevich(name, count, ip));
    });
}

int gygax_network_add_poisson(gygax_network* net, const char* name, size_t count, double rate_hz) {
    return guarded([&] { return static_cast<int>(net->net->addPoisson(name, count, rate_hz)); });
}

int gygax_network_add_spike_source(gygax_network* net, const char* name, size_t count) {
    return guarded([&] { return static_cast<int>(net->net->addSpikeSource(name, count)); });
}

int gygax_network_connect(gygax_network* net, int pre, int post, int kind, double probability, double weight_mean, double weight_std,
                          double delay_ms, int allow_self, const double* stdp) {
    return guarded([&] {
        gygax::neuro::ConnectionSpec cs;
        cs.kind = static_cast<gygax::neuro::Connectivity>(kind);
        cs.probability = probability;
        cs.weightMean = weight_mean;
        cs.weightStd = weight_std;
        cs.delayMs = delay_ms;
        cs.allowSelf = allow_self != 0;
        cs.stdp = stdpFrom(stdp);
        return static_cast<int>(net->net->connect(static_cast<size_t>(pre), static_cast<size_t>(post), cs));
    });
}

int gygax_network_connect_explicit(gygax_network* net, int pre, int post, size_t count, const uint32_t* pre_idx, const uint32_t* post_idx,
                                   const float* weights, double delay_ms, const double* stdp) {
    return guarded([&] {
        std::vector<uint32_t> a(pre_idx, pre_idx + count);
        std::vector<uint32_t> b(post_idx, post_idx + count);
        std::vector<float> w(weights, weights + count);
        return static_cast<int>(
            net->net->connectExplicit(static_cast<size_t>(pre), static_cast<size_t>(post), a, b, w, delay_ms, stdpFrom(stdp)));
    });
}

int gygax_network_set_bias(gygax_network* net, int pop, double current) {
    return guarded([&] {
        net->net->setBias(static_cast<size_t>(pop), current);
        return 0;
    });
}

int gygax_network_set_drive(gygax_network* net, int pop, const double* currents, size_t count) {
    return guarded([&] {
        net->net->setDrive(static_cast<size_t>(pop), std::vector<double>(currents, currents + count));
        return 0;
    });
}

int gygax_network_set_rate(gygax_network* net, int pop, double rate_hz) {
    return guarded([&] {
        net->net->setRate(static_cast<size_t>(pop), rate_hz);
        return 0;
    });
}

int gygax_network_set_rates(gygax_network* net, int pop, const double* rates_hz, size_t count) {
    return guarded([&] {
        net->net->setRates(static_cast<size_t>(pop), std::vector<double>(rates_hz, rates_hz + count));
        return 0;
    });
}

int gygax_network_inject_spike(gygax_network* net, int pop, size_t neuron) {
    return guarded([&] {
        net->net->injectSpike(static_cast<size_t>(pop), neuron);
        return 0;
    });
}

int gygax_network_set_plasticity(gygax_network* net, int enabled) {
    return guarded([&] {
        net->net->setPlasticity(enabled != 0);
        return 0;
    });
}

int gygax_network_set_record_voltage(gygax_network* net, int pop, int enabled) {
    return guarded([&] {
        net->net->setRecordVoltage(static_cast<size_t>(pop), enabled != 0);
        return 0;
    });
}

int gygax_network_run(gygax_network* net, double duration_ms) {
    return guarded([&] {
        net->net->run(duration_ms);
        return 0;
    });
}

int gygax_network_step(gygax_network* net) {
    return guarded([&] {
        net->net->step();
        return 0;
    });
}

int gygax_network_reset(gygax_network* net, int restore_weights) {
    return guarded([&] {
        net->net->reset(restore_weights != 0);
        return 0;
    });
}

int gygax_network_clear_spikes(gygax_network* net) {
    return guarded([&] {
        net->net->clearSpikes();
        return 0;
    });
}

double gygax_network_time_ms(const gygax_network* net) {
    return net->net->timeMs();
}
int gygax_network_population_count(const gygax_network* net) {
    return static_cast<int>(net->net->populationCount());
}
int gygax_network_projection_count(const gygax_network* net) {
    return static_cast<int>(net->net->projectionCount());
}

int gygax_network_find_population(const gygax_network* net, const char* name) {
    auto id = net->net->findPopulation(name);
    return id ? static_cast<int>(*id) : -1;
}

long long gygax_network_population_size(const gygax_network* net, int pop) {
    return guardedLong([&] { return static_cast<long long>(net->net->populationSize(static_cast<size_t>(pop))); });
}

long long gygax_network_spike_count(const gygax_network* net, int pop) {
    return guardedLong([&] { return static_cast<long long>(net->net->spikeCount(static_cast<size_t>(pop))); });
}

double gygax_network_mean_rate_hz(const gygax_network* net, int pop) {
    try {
        return net->net->meanRateHz(static_cast<size_t>(pop));
    } catch (const std::exception& e) {
        setError(e.what());
        return -1.0;
    }
}

long long gygax_network_synapse_count(const gygax_network* net, int proj) {
    return guardedLong([&] { return static_cast<long long>(net->net->synapseCount(static_cast<size_t>(proj))); });
}

double gygax_network_mean_weight(const gygax_network* net, int proj) {
    try {
        return net->net->meanWeight(static_cast<size_t>(proj));
    } catch (const std::exception& e) {
        setError(e.what());
        return 0.0;
    }
}

long long gygax_network_spike_counts(const gygax_network* net, int pop, uint32_t* out, size_t capacity) {
    return guardedLong([&] { return copyOut(net->net->spikeCounts(static_cast<size_t>(pop)), out, capacity); });
}

long long gygax_network_spikes(const gygax_network* net, int pop, double* times_ms, uint32_t* neurons, size_t capacity) {
    return guardedLong([&] {
        const auto& spikes = net->net->spikes(static_cast<size_t>(pop));
        const size_t n = std::min(capacity, spikes.size());
        for (size_t i = 0; i < n; ++i) {
            if (times_ms != nullptr) times_ms[i] = spikes[i].timeMs;
            if (neurons != nullptr) neurons[i] = spikes[i].neuron;
        }
        return static_cast<long long>(spikes.size());
    });
}

long long gygax_network_weights(const gygax_network* net, int proj, float* out, size_t capacity) {
    return guardedLong([&] { return copyOut(net->net->weights(static_cast<size_t>(proj)), out, capacity); });
}

long long gygax_network_membrane(const gygax_network* net, int pop, double* out, size_t capacity) {
    return guardedLong([&] { return copyOut(net->net->membranePotentials(static_cast<size_t>(pop)), out, capacity); });
}

long long gygax_network_voltage_trace(const gygax_network* net, int pop, double* out, size_t capacity) {
    return guardedLong([&] { return copyOut(net->net->voltageTrace(static_cast<size_t>(pop)), out, capacity); });
}

char* gygax_network_describe(const gygax_network* net) {
    return duplicate(net->net->describe().dump());
}

char* gygax_simulate_json(const char* spec_json) {
    if (spec_json == nullptr) {
        setError("spec is null");
        return nullptr;
    }
    std::string error;
    auto spec = gygax::json::parse(spec_json, &error);
    if (!spec) {
        setError("invalid JSON: " + error);
        return nullptr;
    }
    auto out = gygax::neuro::simulateSpec(*spec, &error);
    if (out.isNull()) {
        setError(error);
        return nullptr;
    }
    return duplicate(out.dump());
}

double gygax_lif_rate_hz(double tau_m, double v_rest, double v_reset, double v_thresh, double tau_ref, double resistance,
                         double current_na) {
    gygax::neuro::LifParams p;
    p.tauM = tau_m;
    p.vRest = v_rest;
    p.vReset = v_reset;
    p.vThresh = v_thresh;
    p.tauRef = tau_ref;
    p.resistance = resistance;
    return gygax::neuro::lifSteadyStateRateHz(p, current_na);
}

gygax_vecenv* gygax_vecenv_create(const char* config_json) {
    try {
        std::string error;
        auto spec = gygax::json::parse(config_json != nullptr ? config_json : "{}", &error);
        if (!spec) {
            setError("invalid JSON: " + error);
            return nullptr;
        }
        auto config = gygax::sim::vecEnvConfigFromJson(*spec, &error);
        if (!config) {
            setError(error);
            return nullptr;
        }
        return new gygax_vecenv{std::make_unique<gygax::sim::VecEnv>(*config)};
    } catch (const std::exception& e) {
        setError(e.what());
        return nullptr;
    }
}

void gygax_vecenv_destroy(gygax_vecenv* env) {
    delete env;
}

long long gygax_vecenv_num_envs(const gygax_vecenv* env) {
    return env != nullptr ? static_cast<long long>(env->env->numEnvs()) : -1;
}

long long gygax_vecenv_observation_size(const gygax_vecenv* env) {
    return env != nullptr ? static_cast<long long>(env->env->observationSize()) : -1;
}

long long gygax_vecenv_action_size(const gygax_vecenv* env) {
    return env != nullptr ? static_cast<long long>(env->env->actionSize()) : -1;
}

int gygax_vecenv_reset(gygax_vecenv* env, uint64_t seed) {
    return guarded([&] {
        env->env->reset(seed);
        return 0;
    });
}

int gygax_vecenv_step(gygax_vecenv* env, const float* actions, size_t action_count) {
    return guarded([&] {
        if (actions == nullptr || action_count != env->env->numEnvs() * env->env->actionSize())
            throw std::invalid_argument("actions must hold num_envs * action_size floats");
        env->env->step(actions);
        return 0;
    });
}

const float* gygax_vecenv_observations(const gygax_vecenv* env) {
    return env->env->observations().data();
}
const float* gygax_vecenv_final_observations(const gygax_vecenv* env) {
    return env->env->finalObservations().data();
}
const float* gygax_vecenv_rewards(const gygax_vecenv* env) {
    return env->env->rewards().data();
}
const uint8_t* gygax_vecenv_terminated(const gygax_vecenv* env) {
    return env->env->terminated().data();
}
const uint8_t* gygax_vecenv_truncated(const gygax_vecenv* env) {
    return env->env->truncated().data();
}
const uint8_t* gygax_vecenv_collided(const gygax_vecenv* env) {
    return env->env->collided().data();
}
const uint8_t* gygax_vecenv_reached_goal(const gygax_vecenv* env) {
    return env->env->reachedGoal().data();
}
}
