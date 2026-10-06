import sys

from gygax import neuro

STDP_POTENTIATION_RATE = 0.5
STDP_DEPRESSION_RATE = 0.5
MINIMUM_SYNAPTIC_WEIGHT = 0.0
MAXIMUM_SYNAPTIC_WEIGHT = 400.0
NETWORK_TIMESTEP_MS = 0.1
NETWORK_SEED = 3
NEURON_COUNT = 1
STRONG_SYNAPTIC_WEIGHT = 120.0
WEAK_SYNAPTIC_WEIGHT = 2.0
SYNAPTIC_DELAY_MS = 1.0
PAIRING_REPETITIONS = 10
CAUSAL_INTERVAL_MS = 30
ANTICAUSAL_INTERVAL_MS = 6


def learning_rule():
    return neuro.Stdp(
        a_plus=STDP_POTENTIATION_RATE,
        a_minus=STDP_DEPRESSION_RATE,
        w_min=MINIMUM_SYNAPTIC_WEIGHT,
        w_max=MAXIMUM_SYNAPTIC_WEIGHT,
    )


def causal_pairing():
    stdp = learning_rule()
    with neuro.Network(dt_ms=NETWORK_TIMESTEP_MS, seed=NETWORK_SEED) as net:
        pre = net.add_spike_source("pre", NEURON_COUNT)
        post = net.add_lif("post", NEURON_COUNT)
        proj = net.connect_explicit(pre, post, [0], [0], [STRONG_SYNAPTIC_WEIGHT],
                                    delay_ms=SYNAPTIC_DELAY_MS, stdp=stdp)
        before = proj.mean_weight
        for _ in range(PAIRING_REPETITIONS):
            pre.inject_spike(0)
            net.run(CAUSAL_INTERVAL_MS)
        return before, proj.mean_weight


def anticausal_pairing():
    stdp = learning_rule()
    with neuro.Network(dt_ms=NETWORK_TIMESTEP_MS, seed=NETWORK_SEED) as net:
        trigger = net.add_spike_source("trigger", NEURON_COUNT)
        pre = net.add_spike_source("pre", NEURON_COUNT)
        post = net.add_lif("post", NEURON_COUNT)
        net.connect_explicit(trigger, post, [0], [0], [STRONG_SYNAPTIC_WEIGHT], delay_ms=SYNAPTIC_DELAY_MS)
        proj = net.connect_explicit(pre, post, [0], [0], [WEAK_SYNAPTIC_WEIGHT],
                                    delay_ms=SYNAPTIC_DELAY_MS, stdp=stdp)
        before = proj.mean_weight
        for _ in range(PAIRING_REPETITIONS):
            trigger.inject_spike(0)
            net.run(ANTICAUSAL_INTERVAL_MS)
            pre.inject_spike(0)
            net.run(CAUSAL_INTERVAL_MS)
        return before, proj.mean_weight


def main():
    print(f"gygax {neuro.version()}")
    before, after = causal_pairing()
    print(f"pre before post : weight {before:.2f} -> {after:.2f} (potentiation)")
    ok = after > before
    before, after = anticausal_pairing()
    print(f"post before pre : weight {before:.2f} -> {after:.2f} (depression)")
    ok = ok and after < before
    print("STDP OK" if ok else "STDP FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
