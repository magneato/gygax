import ctypes
import json

import numpy as np

from ._native import lib

NON_LIDAR_OBSERVATION_FEATURES = 4
DIFFERENTIAL_DRIVE_ACTION_COUNT = 2

try:
    import gymnasium as gym
    from gymnasium import spaces
except ImportError:
    gym = None
    spaces = None


class VecEnv:
    def __init__(self, **config):
        self._L = lib()
        self._h = self._L.gygax_vecenv_create(json.dumps(config).encode())
        if not self._h:
            raise ValueError(self._L.gygax_last_error().decode())
        self.num_envs = self._L.gygax_vecenv_num_envs(self._h)
        self.observation_size = self._L.gygax_vecenv_observation_size(self._h)
        self.action_size = self._L.gygax_vecenv_action_size(self._h)
        self.beams = self.observation_size - NON_LIDAR_OBSERVATION_FEATURES

    def close(self):
        if getattr(self, "_h", None):
            self._L.gygax_vecenv_destroy(self._h)
            self._h = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        self.close()

    def _view(self, getter, dtype, count):
        ptr = getattr(self._L, "gygax_vecenv_" + getter)(self._h)
        ctype = ctypes.c_float if dtype == np.float32 else ctypes.c_uint8
        return np.ctypeslib.as_array(ctypes.cast(ptr, ctypes.POINTER(ctype)), shape=(count,)).copy()

    def _obs(self, getter):
        return self._view(getter, np.float32, self.num_envs * self.observation_size).reshape(self.num_envs, self.observation_size)

    def reset(self, seed=0):
        if self._L.gygax_vecenv_reset(self._h, int(seed)) != 0:
            raise RuntimeError(self._L.gygax_last_error().decode())
        return self._obs("observations"), {}

    def step(self, actions):
        actions = np.ascontiguousarray(actions, dtype=np.float32).reshape(-1)
        ptr = actions.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        if self._L.gygax_vecenv_step(self._h, ptr, actions.size) != 0:
            raise RuntimeError(self._L.gygax_last_error().decode())
        n = self.num_envs
        info = {
            "final_observation": self._obs("final_observations"),
            "collided": self._view("collided", np.uint8, n).astype(bool),
            "reached_goal": self._view("reached_goal", np.uint8, n).astype(bool),
        }
        return (
            self._obs("observations"),
            self._view("rewards", np.float32, n),
            self._view("terminated", np.uint8, n).astype(bool),
            self._view("truncated", np.uint8, n).astype(bool),
            info,
        )


_Base = gym.Env if gym is not None else object


class GygaxRoverEnv(_Base):
    metadata = {"render_modes": []}

    def __init__(self, **config):
        config["num_envs"] = 1
        config["auto_reset"] = False
        self._config = config
        self._vec = VecEnv(**config)
        self._seed = int(config.get("seed", 0))
        if spaces is not None:
            self.observation_space = spaces.Box(-1.0, 1.0, shape=(self._vec.observation_size,), dtype=np.float32)
            self.action_space = spaces.Box(-1.0, 1.0, shape=(self._vec.action_size,), dtype=np.float32)

    def reset(self, *, seed=None, options=None):
        if gym is not None:
            super().reset(seed=seed)
        if seed is not None:
            self._seed = int(seed)
        obs, _ = self._vec.reset(self._seed)
        self._seed += 1
        return obs[0], {}

    def step(self, action):
        obs, reward, terminated, truncated, info = self._vec.step(np.asarray(action, dtype=np.float32).reshape(1, -1))
        return obs[0], float(reward[0]), bool(terminated[0]), bool(truncated[0]), {
            "collided": bool(info["collided"][0]),
            "reached_goal": bool(info["reached_goal"][0]),
        }

    def close(self):
        self._vec.close()
