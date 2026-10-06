import unittest

try:
    import numpy as np
except ImportError:
    raise unittest.SkipTest("numpy is not installed")

from gygax import rl


class VecEnvTests(unittest.TestCase):
    def test_shapes_and_step(self):
        with rl.VecEnv(num_envs=5, beams=8, seed=3) as env:
            self.assertEqual((env.num_envs, env.observation_size, env.action_size), (5, 12, 2))
            obs, _ = env.reset(3)
            self.assertEqual(obs.shape, (5, 12))
            self.assertEqual(obs.dtype, np.float32)
            obs, reward, terminated, truncated, info = env.step(np.zeros((5, 2)))
            self.assertEqual(reward.shape, (5,))
            self.assertEqual(terminated.dtype, bool)
            self.assertEqual(info["final_observation"].shape, (5, 12))

    def test_same_seed_same_trajectory_for_any_thread_count(self):
        def rollout(threads):
            with rl.VecEnv(num_envs=8, threads=threads, seed=9, randomization={"motor_gain": [0.8, 1.2], "lidar_noise_std": [0, 0.1]}) as env:
                env.reset(9)
                rng = np.random.default_rng(0)
                out = []
                for _ in range(60):
                    obs, reward, *_ = env.step(rng.uniform(-1, 1, size=(8, 2)))
                    out.append(np.concatenate([obs.ravel(), reward]))
                return np.concatenate(out)

        np.testing.assert_array_equal(rollout(1), rollout(4))

    def test_truncation_and_autoreset(self):
        with rl.VecEnv(num_envs=2, max_steps=4, seed=1) as env:
            env.reset(1)
            for _ in range(3):
                _, _, _, truncated, _ = env.step(np.zeros((2, 2)))
                self.assertFalse(truncated.any())
            _, _, _, truncated, _ = env.step(np.zeros((2, 2)))
            self.assertTrue(truncated.all())

    def test_bad_input_is_reported(self):
        with self.assertRaises(ValueError):
            rl.VecEnv(num_envs=0)
        with self.assertRaises(ValueError):
            rl.VecEnv(randomization={"motor_gain": [2, 1]})
        with rl.VecEnv(num_envs=2) as env:
            with self.assertRaises(RuntimeError):
                env.step(np.zeros((3, 2)))


class GymApiTests(unittest.TestCase):
    def test_single_env_follows_the_gymnasium_contract(self):
        env = rl.GygaxRoverEnv(seed=5, max_steps=10, beams=8)
        obs, info = env.reset(seed=5)
        self.assertEqual(obs.shape, (12,))
        seen_truncation = False
        for _ in range(10):
            obs, reward, terminated, truncated, info = env.step(np.array([0.5, 0.1], dtype=np.float32))
            self.assertIsInstance(reward, float)
            seen_truncation = seen_truncation or truncated
            if terminated or truncated:
                break
        self.assertTrue(seen_truncation or terminated)
        first, _ = env.reset(seed=5)
        again, _ = env.reset(seed=5)
        np.testing.assert_array_equal(first, again)
        env.close()

    @unittest.skipUnless(rl.gym is not None, "gymnasium is not installed")
    def test_passes_the_gymnasium_checker(self):
        from gymnasium.utils.env_checker import check_env

        check_env(rl.GygaxRoverEnv(seed=1, max_steps=20))


if __name__ == "__main__":
    unittest.main()
