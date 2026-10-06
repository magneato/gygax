import math
import unittest

import gygax
from gygax import neuro


class NetworkTests(unittest.TestCase):
    def test_version_matches_package(self):
        self.assertEqual(neuro.version(), gygax.__version__)

    def test_lif_rate_matches_analytic_curve(self):
        params = neuro.LifParams()
        for current in (16.0, 20.0, 30.0):
            with neuro.Network(dt_ms=0.05, seed=1) as net:
                pop = net.add_lif("n", 1, params)
                pop.set_bias(current)
                net.run(4000)
                expected = params.steady_state_rate_hz(current)
                self.assertAlmostEqual(pop.mean_rate_hz, expected, delta=0.05 * expected + 0.5)

    def test_same_seed_same_raster(self):
        def run():
            with neuro.Network(dt_ms=0.1, seed=11) as net:
                src = net.add_poisson("in", 40, 60.0)
                dst = net.add_lif("out", 40)
                net.connect(src, dst, p=0.3, weight=8.0, weight_std=1.0)
                net.run(300)
                times, neurons = dst.spikes()
                return list(times), list(neurons)

        first, second = run(), run()
        self.assertEqual(first, second)
        self.assertGreater(len(first[0]), 0)

    def test_stdp_potentiates_causal_pairing(self):
        with neuro.Network(dt_ms=0.1, seed=3) as net:
            pre = net.add_spike_source("pre", 1)
            post = net.add_lif("post", 1)
            proj = net.connect_explicit(
                pre, post, [0], [0], [120.0], delay_ms=1.0, stdp=neuro.Stdp(a_plus=0.5, a_minus=0.5, w_min=0.0, w_max=400.0)
            )
            before = proj.mean_weight
            for _ in range(10):
                pre.inject_spike(0)
                net.run(30)
            self.assertEqual(post.spike_count, 10)
            self.assertGreater(proj.mean_weight, before)

    def test_stdp_depresses_anticausal_pairing(self):
        with neuro.Network(dt_ms=0.1, seed=3) as net:
            trigger = net.add_spike_source("trigger", 1)
            pre = net.add_spike_source("pre", 1)
            post = net.add_lif("post", 1)
            net.connect_explicit(trigger, post, [0], [0], [120.0], delay_ms=1.0)
            proj = net.connect_explicit(
                pre, post, [0], [0], [2.0], delay_ms=1.0, stdp=neuro.Stdp(a_plus=0.5, a_minus=0.5, w_min=0.0, w_max=400.0)
            )
            before = proj.mean_weight
            for _ in range(10):
                trigger.inject_spike(0)
                net.run(6)
                pre.inject_spike(0)
                net.run(30)
            self.assertEqual(post.spike_count, 10)
            self.assertLess(proj.mean_weight, before)
            self.assertGreaterEqual(before - proj.mean_weight, 1.0)

    def test_plasticity_can_be_frozen(self):
        with neuro.Network(dt_ms=0.1, seed=3) as net:
            pre = net.add_spike_source("pre", 1)
            post = net.add_lif("post", 1)
            proj = net.connect_explicit(
                pre, post, [0], [0], [120.0], delay_ms=1.0, stdp=neuro.Stdp(a_plus=0.5, a_minus=0.5, w_min=0.0, w_max=400.0)
            )
            net.set_plasticity(False)
            for _ in range(5):
                pre.inject_spike(0)
                net.run(30)
            self.assertEqual(proj.mean_weight, 120.0)

    def test_spec_run_matches_builder(self):
        spec = {
            "dt": 0.1,
            "seed": 5,
            "run_ms": 200,
            "populations": [
                {"name": "in", "type": "poisson", "n": 20, "rate": 50},
                {"name": "out", "type": "lif", "n": 20},
            ],
            "projections": [{"pre": "in", "post": "out", "connect": {"type": "fixed_prob", "p": 0.4}, "weight": 9.0}],
        }
        result = neuro.simulate(spec)
        with neuro.Network.from_spec(spec) as net:
            net.run(200)
            self.assertEqual(net.population("out").spike_count, result["results"][1]["spike_count"])

    def test_invalid_input_raises(self):
        with neuro.Network() as net:
            with self.assertRaises(neuro.GygaxError):
                net.add_lif("bad", 0)
            with self.assertRaises(neuro.GygaxError):
                net.run(-1)

    def test_readout_argmax(self):
        with neuro.Network(dt_ms=0.1, seed=2) as net:
            src = net.add_poisson("in", 2, 0.0)
            out = net.add_lif("out", 2)
            net.connect_explicit(src, out, [0, 1], [0, 1], [12.0, 12.0], delay_ms=1.0)
            src.set_rates([200.0, 0.0])
            net.run(500)
            counts = list(out.spike_counts())
            self.assertGreater(counts[0], counts[1])


if __name__ == "__main__":
    unittest.main()
