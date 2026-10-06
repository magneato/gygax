# Neuromorphic simulation

The simulator is a clock-driven spiking network engine with exact-exponential membrane updates, delayed sparse synapses and pair-based STDP. It is deterministic: the same spec and seed always produce the same spike raster, on the same build.

## Models

Times are in milliseconds, potentials in mV, currents in nA, resistance in MΩ, rates in Hz.

**LIF** (`lif`): `dv/dt = (-(v - v_rest) + R (I_syn + I_bias)) / tau_m`, integrated exactly over each step for piecewise-constant input: `v <- v_inf + (v - v_inf) exp(-dt/tau_m)` with `v_inf = v_rest + R I`. At `v >= v_thresh` the neuron spikes, resets to `v_reset` and is refractory for `ceil(tau_ref/dt)` steps. Parameters and defaults: `tau_m 20`, `v_rest -65`, `v_reset -65`, `v_thresh -50`, `tau_ref 2`, `tau_syn 5`, `resistance 1`. `I_syn` decays exponentially with `tau_syn` and jumps by the synaptic weight (nA) on each arriving spike.

For a constant current the steady rate is `1000 / (tau_ref + tau_m ln((v_inf - v_reset)/(v_inf - v_thresh)))` when `v_inf > v_thresh`. `neuro::lifSteadyStateRateHz` computes it, and the tests check the simulator against it within 5 % for currents from 16 to 40 nA.

**Izhikevich** (`izhikevich`): `v' = 0.04 v² + 5 v + 140 - u + I`, `u' = a (b v - u)`, reset `v <- c, u <- u + d` at 30 mV, integrated with sub-steps of at most 0.5 ms. Defaults are regular spiking (`a 0.02, b 0.2, c -65, d 8`).

**Poisson** (`poisson`): each neuron fires with probability `rate * dt / 1000` per step. Rates can be set per neuron at any time (`set_rates`), which is how sensors are encoded.

**Spike source** (`spike_source`): fires exactly when you call `inject_spike`.

## Connectivity and plasticity

Projections are `all_to_all`, `one_to_one` or `fixed_prob`, or fully explicit (`connect_explicit`). Weights may be drawn from a normal distribution (clipped to the sign of the mean, so excitatory stays excitatory) and share one axonal delay per projection, rounded to whole steps with a minimum of one. Negative weights are inhibitory.

STDP is the additive pair-based rule with exponential traces: on a presynaptic spike `w -= a_minus * post_trace`; on a postsynaptic spike `w += a_plus * pre_trace`; weights are clipped to `[w_min, w_max]`. Defaults: `a_plus 0.01`, `a_minus 0.0105`, `tau_plus = tau_minus = 20`. `set_plasticity(false)` freezes learning without removing the rule. `reset()` restores the initial weights unless told otherwise.

## Determinism and limits

- Randomness comes from xoshiro256** seeded through splitmix64; connectivity generation and Poisson sampling use the network's single stream, so results depend on construction order.
- Spike recording is capped at 5,000,000 events per network (`dropped_spikes` reports overflow).
- A spec run is limited to 64 populations and 200,000 neurons and 600,000 ms; the library API has no such limits beyond memory.
- Performance on one core (measured, RelWithDebInfo): 10,000 neurons and 2.3 M synapses at `dt = 0.1 ms` simulate 1 s of activity in about 1.4 s including construction. Cost scales with spikes times fan-out, so sparse activity is cheap.

## Interfaces

### C++

```cpp
#include <gygax/neuro/network.hpp>
using namespace gygax::neuro;

Network net(0.1, /*seed*/ 1);
auto in  = net.addPoisson("in", 100, 30.0);
auto out = net.addLif("out", 50);
ConnectionSpec spec; spec.probability = 0.2; spec.weightMean = 6.0; spec.stdp = StdpParams{};
net.connect(in, out, spec);
net.run(1000.0);
double rate = net.meanRateHz(out);
```

### JSON spec (CLI, HTTP, agent tool)

```json
{
  "dt": 0.1, "seed": 42, "run_ms": 1000, "raster_limit": 100,
  "populations": [
    {"name": "in",  "type": "poisson", "n": 100, "rate": 40},
    {"name": "out", "type": "lif", "n": 10, "params": {"tau_m": 30}, "bias": 0.0, "record_voltage": true}
  ],
  "projections": [
    {"pre": "in", "post": "out", "connect": {"type": "fixed_prob", "p": 0.2},
     "weight": {"mean": 6.0, "std": 1.0}, "delay_ms": 1.0,
     "stdp": {"a_plus": 0.01, "a_minus": 0.0105, "w_min": 0, "w_max": 12}}
  ]
}
```

`gygax neuro spec.json`, `gygax neuro demo`, `POST /v1/neuro/simulate`, or the `neuro.run` tool return per-population spike counts and mean rates, per-projection synapse counts and mean weights, and up to `raster_limit` `[time_ms, neuron]` pairs per population.

### C ABI

`include/gygax/capi/gygax.h`, library `libgygax_c` (`.so`, `.dylib`). All calls return a negative value on error and set a thread-local message readable through `gygax_last_error()`. Buffers are caller-owned: getters copy at most `capacity` elements and return the total available, so call once with capacity 0 to size the buffer.

### Python

`python/gygax` (Python 3.9+, numpy optional) wraps the C ABI with `Network`, `Population`, `Projection`, `LifParams`, `IzhikevichParams`, `Stdp` and `simulate(spec)`. Set `GYGAX_LIB` to the shared library if it is not found automatically. `gygax.Client` talks to a running service. See `examples/python/`.

### Hardware node

`hardware::NeuromorphicNode` embeds a network in the node hierarchy: `bindInput`, `encode(values, max_rate_hz)`, `step(dt_ms)`, `readout()`, `decision()`. `examples/arrival` drives a rover with a two-neuron Braitenberg controller built this way.

## Validation

`tests/unit/test_neuro.cpp` and `python/tests/test_neuro.py` cover: RNG moments, the analytic LIF rate at six currents, refractory limits, sub-threshold settling, Poisson rate accuracy, synaptic delays, inhibition, connectivity counts, argument validation, determinism, reset, causal potentiation and anticausal depression, weight bounds, frozen plasticity, voltage recording, encoding and spec error messages.
