#ifndef GYGAX_CAPI_H
#define GYGAX_CAPI_H

/*
 * Gygax C API
 *
 * This header exposes the supported C interface for neuromorphic networks
 * and vectorized simulation environments. Handles are opaque; callers own
 * each successful handle returned by a create function and must destroy it.
 *
 * Versioning: gygax_version() reports the library's project version.
 * Gygax is pre-1.0: source and binary compatibility are not promised between
 * releases. Check the release notes and rebuild consumers when upgrading.
 *
 * Trust boundary: this API does not authorize or control physical hardware.
 * Simulation results are not evidence of real-world safety.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define GYGAX_API __declspec(dllexport)
#else
#define GYGAX_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle to a spiking-neural-network simulation. */
typedef struct gygax_network gygax_network;

/** Connectivity patterns accepted by gygax_network_connect(). */
enum gygax_connectivity { GYGAX_ALL_TO_ALL = 0, GYGAX_ONE_TO_ONE = 1, GYGAX_FIXED_PROBABILITY = 2 };

/** Return the library's project version string (owned by the library). */
GYGAX_API const char* gygax_version(void);
/**
 * Return the most recent error for the calling thread, if available.
 * The returned string is owned by the library and may change after another
 * API call on the same thread.
 */
GYGAX_API const char* gygax_last_error(void);
/** Free a buffer returned by a Gygax API function; NULL is accepted. */
GYGAX_API void gygax_free(void* ptr);

/**
 * Create a network with timestep dt_ms and deterministic random seed.
 * Returns NULL on failure; inspect gygax_last_error() on the same thread.
 */
GYGAX_API gygax_network* gygax_network_create(double dt_ms, uint64_t seed);
/** Construct a network from a JSON specification; NULL indicates failure. */
GYGAX_API gygax_network* gygax_network_from_json(const char* spec_json);
/** Destroy a network handle returned by a create function; NULL is accepted. */
GYGAX_API void gygax_network_destroy(gygax_network* net);

/** Add a population. The params array follows the model-specific parameter order. */
GYGAX_API int gygax_network_add_lif(gygax_network* net, const char* name, size_t count, const double* params);
GYGAX_API int gygax_network_add_izhikevich(gygax_network* net, const char* name, size_t count, const double* params);
GYGAX_API int gygax_network_add_poisson(gygax_network* net, const char* name, size_t count, double rate_hz);
GYGAX_API int gygax_network_add_spike_source(gygax_network* net, const char* name, size_t count);

/** Connect populations using a selected pattern and optional STDP parameters. */
GYGAX_API int gygax_network_connect(gygax_network* net, int pre, int post, int kind, double probability, double weight_mean,
                                    double weight_std, double delay_ms, int allow_self, const double* stdp);
/** Connect explicit neuron-index pairs with the supplied weights and delay. */
GYGAX_API int gygax_network_connect_explicit(gygax_network* net, int pre, int post, size_t count, const uint32_t* pre_idx,
                                             const uint32_t* post_idx, const float* weights, double delay_ms, const double* stdp);

/** Set one population's input bias, per-neuron drive, or Poisson rate. */
GYGAX_API int gygax_network_set_bias(gygax_network* net, int pop, double current);
GYGAX_API int gygax_network_set_drive(gygax_network* net, int pop, const double* currents, size_t count);
GYGAX_API int gygax_network_set_rate(gygax_network* net, int pop, double rate_hz);
GYGAX_API int gygax_network_set_rates(gygax_network* net, int pop, const double* rates_hz, size_t count);
/** Inject one spike or enable/disable network recording behavior. */
GYGAX_API int gygax_network_inject_spike(gygax_network* net, int pop, size_t neuron);
GYGAX_API int gygax_network_set_plasticity(gygax_network* net, int enabled);
GYGAX_API int gygax_network_set_record_voltage(gygax_network* net, int pop, int enabled);

/** Advance the network by duration_ms or by one timestep. */
GYGAX_API int gygax_network_run(gygax_network* net, double duration_ms);
GYGAX_API int gygax_network_step(gygax_network* net);
/** Reset simulation state; restore_weights controls restoration of initial weights. */
GYGAX_API int gygax_network_reset(gygax_network* net, int restore_weights);
GYGAX_API int gygax_network_clear_spikes(gygax_network* net);

/** Read-only network and population/projection summary queries. */
GYGAX_API double gygax_network_time_ms(const gygax_network* net);
GYGAX_API int gygax_network_population_count(const gygax_network* net);
GYGAX_API int gygax_network_projection_count(const gygax_network* net);
GYGAX_API int gygax_network_find_population(const gygax_network* net, const char* name);
GYGAX_API long long gygax_network_population_size(const gygax_network* net, int pop);
GYGAX_API long long gygax_network_spike_count(const gygax_network* net, int pop);
GYGAX_API double gygax_network_mean_rate_hz(const gygax_network* net, int pop);
GYGAX_API long long gygax_network_synapse_count(const gygax_network* net, int proj);
GYGAX_API double gygax_network_mean_weight(const gygax_network* net, int proj);

/**
 * Copy result arrays into caller-provided storage.
 * Capacity is measured in elements; the return value is the total number of
 * available elements, so callers can size buffers before copying.
 */
GYGAX_API long long gygax_network_spike_counts(const gygax_network* net, int pop, uint32_t* out, size_t capacity);
GYGAX_API long long gygax_network_spikes(const gygax_network* net, int pop, double* times_ms, uint32_t* neurons, size_t capacity);
GYGAX_API long long gygax_network_weights(const gygax_network* net, int proj, float* out, size_t capacity);
GYGAX_API long long gygax_network_membrane(const gygax_network* net, int pop, double* out, size_t capacity);
GYGAX_API long long gygax_network_voltage_trace(const gygax_network* net, int pop, double* out, size_t capacity);

/** Returned strings are allocated by Gygax and must be released with gygax_free(). */
GYGAX_API char* gygax_network_describe(const gygax_network* net);
GYGAX_API char* gygax_simulate_json(const char* spec_json);
/** Estimate the steady-state firing rate for a leaky integrate-and-fire model. */
GYGAX_API double gygax_lif_rate_hz(double tau_m, double v_rest, double v_reset, double v_thresh, double tau_ref, double resistance,
                                   double current_na);

/** Opaque handle to a batch of independent simulation environments. */
typedef struct gygax_vecenv gygax_vecenv;

/** Create a vector environment from JSON; NULL indicates failure. */
GYGAX_API gygax_vecenv* gygax_vecenv_create(const char* config_json);
/** Destroy an environment handle returned by gygax_vecenv_create(); NULL is accepted. */
GYGAX_API void gygax_vecenv_destroy(gygax_vecenv* env);
/** Query environment, observation, and action dimensions. */
GYGAX_API long long gygax_vecenv_num_envs(const gygax_vecenv* env);
GYGAX_API long long gygax_vecenv_observation_size(const gygax_vecenv* env);
GYGAX_API long long gygax_vecenv_action_size(const gygax_vecenv* env);
/** Reset all environments using the supplied seed. */
GYGAX_API int gygax_vecenv_reset(gygax_vecenv* env, uint64_t seed);
/** Advance all environments using a flat action array. */
GYGAX_API int gygax_vecenv_step(gygax_vecenv* env, const float* actions, size_t action_count);
/**
 * Return read-only, library-owned arrays for the latest step.
 * Pointers remain owned by the environment and may be invalidated by its next
 * reset, step, or destruction; do not free or retain them past that point.
 */
GYGAX_API const float* gygax_vecenv_observations(const gygax_vecenv* env);
GYGAX_API const float* gygax_vecenv_final_observations(const gygax_vecenv* env);
GYGAX_API const float* gygax_vecenv_rewards(const gygax_vecenv* env);
/** Per-environment flags from the most recent step; same lifetime as observations. */
GYGAX_API const uint8_t* gygax_vecenv_terminated(const gygax_vecenv* env);
GYGAX_API const uint8_t* gygax_vecenv_truncated(const gygax_vecenv* env);
GYGAX_API const uint8_t* gygax_vecenv_collided(const gygax_vecenv* env);
GYGAX_API const uint8_t* gygax_vecenv_reached_goal(const gygax_vecenv* env);

#ifdef __cplusplus
}
#endif

#endif
