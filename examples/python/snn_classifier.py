import sys

import numpy as np

from gygax import neuro

CLASS_COUNT = 4
INPUT_COUNT = 80
RANDOM_SEED = 0
NETWORK_SEED = 5
NETWORK_TIMESTEP_MS = 0.1
NOISE_PROBABILITY = 0.15
SYNAPTIC_WEIGHT = 4.0
BACKGROUND_WEIGHT = 0.1
SYNAPTIC_DELAY_MS = 1.0
TRAINING_TRIAL_COUNT = 200
INPUT_MAX_RATE_HZ = 60.0
CLASSIFICATION_WINDOW_MS = 100.0
MINIMUM_ACCURACY = 0.9


def make_prototypes(n_classes, n_inputs, rng):
    protos = np.zeros((n_classes, n_inputs))
    block = n_inputs // n_classes
    for c in range(n_classes):
        protos[c, c * block : (c + 1) * block] = 1.0
    return protos


def sample(protos, cls, rng, noise=NOISE_PROBABILITY):
    x = protos[cls].copy()
    flip = rng.random(x.shape) < noise
    return np.where(flip, 1.0 - x, x)


def classify(net, inputs, out, x, max_rate_hz, window_ms):
    inputs.set_rates([r for r in x * max_rate_hz])
    net.clear_spikes()
    net.run(window_ms)
    counts = out.spike_counts()
    return int(np.argmax(counts)), counts


def main():
    n_classes, n_inputs = CLASS_COUNT, INPUT_COUNT
    rng = np.random.default_rng(RANDOM_SEED)
    protos = make_prototypes(n_classes, n_inputs, rng)

    with neuro.Network(dt_ms=NETWORK_TIMESTEP_MS, seed=NETWORK_SEED) as net:
        inputs = net.add_poisson("inputs", n_inputs, 0.0)
        out = net.add_lif("out", n_classes)
        block = n_inputs // n_classes
        pre, post, weights = [], [], []
        for c in range(n_classes):
            for i in range(n_inputs):
                pre.append(i)
                post.append(c)
                weights.append(SYNAPTIC_WEIGHT if c * block <= i < (c + 1) * block else BACKGROUND_WEIGHT)
        net.connect_explicit(inputs, out, pre, post, weights, delay_ms=SYNAPTIC_DELAY_MS)

        trials, correct = TRAINING_TRIAL_COUNT, 0
        for _ in range(trials):
            cls = int(rng.integers(n_classes))
            guess, _ = classify(net, inputs, out, sample(protos, cls, rng),
                                INPUT_MAX_RATE_HZ, CLASSIFICATION_WINDOW_MS)
            correct += guess == cls
        accuracy = correct / trials
        print(f"spiking readout accuracy on noisy prototypes: {accuracy:.1%} ({correct}/{trials})")
        print("SNN CLASSIFIER OK" if accuracy > MINIMUM_ACCURACY else "SNN CLASSIFIER FAILED")
        return 0 if accuracy > MINIMUM_ACCURACY else 1


if __name__ == "__main__":
    sys.exit(main())
