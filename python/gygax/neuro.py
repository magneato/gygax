import ctypes
import json
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from ._native import lib

try:
    import numpy as _np
except ImportError:
    _np = None

ALL_TO_ALL = 0
ONE_TO_ONE = 1
FIXED_PROBABILITY = 2
DEFAULT_LIF_TAU_MEMBRANE_MS = 20.0
DEFAULT_LIF_RESTING_POTENTIAL_MV = -65.0
DEFAULT_LIF_RESET_POTENTIAL_MV = -65.0
DEFAULT_LIF_THRESHOLD_POTENTIAL_MV = -50.0
DEFAULT_LIF_REFRACTORY_PERIOD_MS = 2.0
DEFAULT_SYNAPTIC_TIME_CONSTANT_MS = 5.0
DEFAULT_MEMBRANE_RESISTANCE = 1.0
DEFAULT_IZHIKEVICH_A = 0.02
DEFAULT_IZHIKEVICH_B = 0.2
DEFAULT_IZHIKEVICH_C_MV = -65.0
DEFAULT_IZHIKEVICH_D = 8.0
DEFAULT_STDP_A_PLUS = 0.01
DEFAULT_STDP_A_MINUS = 0.0105
DEFAULT_STDP_TAU_PLUS_MS = 20.0
DEFAULT_STDP_TAU_MINUS_MS = 20.0
DEFAULT_STDP_MAX_WEIGHT = 1.0
DEFAULT_NETWORK_TIMESTEP_MS = 0.1
DEFAULT_RANDOM_SEED = 1
DEFAULT_CONNECTION_PROBABILITY = 0.1
DEFAULT_CONNECTION_WEIGHT = 1.0
DEFAULT_CONNECTION_DELAY_MS = 1.0
DEFAULT_MAX_RATE_HZ = 100.0


class GygaxError(RuntimeError):
    pass


def _check(rc: int) -> int:
    if rc < 0:
        raise GygaxError(lib().gygax_last_error().decode("utf-8", "replace"))
    return rc


def _take_string(ptr) -> str:
    if not ptr:
        raise GygaxError(lib().gygax_last_error().decode("utf-8", "replace"))
    try:
        return ctypes.string_at(ptr).decode("utf-8")
    finally:
        lib().gygax_free(ptr)


def version() -> str:
    return lib().gygax_version().decode()


@dataclass(frozen=True)
class LifParams:
    tau_m: float = DEFAULT_LIF_TAU_MEMBRANE_MS
    v_rest: float = DEFAULT_LIF_RESTING_POTENTIAL_MV
    v_reset: float = DEFAULT_LIF_RESET_POTENTIAL_MV
    v_thresh: float = DEFAULT_LIF_THRESHOLD_POTENTIAL_MV
    tau_ref: float = DEFAULT_LIF_REFRACTORY_PERIOD_MS
    tau_syn: float = DEFAULT_SYNAPTIC_TIME_CONSTANT_MS
    resistance: float = DEFAULT_MEMBRANE_RESISTANCE

    def _array(self):
        values = (self.tau_m, self.v_rest, self.v_reset, self.v_thresh, self.tau_ref, self.tau_syn, self.resistance)
        return (ctypes.c_double * 7)(*values)

    def steady_state_rate_hz(self, current_na: float) -> float:
        return lib().gygax_lif_rate_hz(
            self.tau_m, self.v_rest, self.v_reset, self.v_thresh, self.tau_ref, self.resistance, current_na
        )


@dataclass(frozen=True)
class IzhikevichParams:
    a: float = DEFAULT_IZHIKEVICH_A
    b: float = DEFAULT_IZHIKEVICH_B
    c: float = DEFAULT_IZHIKEVICH_C_MV
    d: float = DEFAULT_IZHIKEVICH_D
    tau_syn: float = DEFAULT_SYNAPTIC_TIME_CONSTANT_MS

    def _array(self):
        return (ctypes.c_double * 5)(self.a, self.b, self.c, self.d, self.tau_syn)


@dataclass(frozen=True)
class Stdp:
    a_plus: float = DEFAULT_STDP_A_PLUS
    a_minus: float = DEFAULT_STDP_A_MINUS
    tau_plus: float = DEFAULT_STDP_TAU_PLUS_MS
    tau_minus: float = DEFAULT_STDP_TAU_MINUS_MS
    w_min: float = 0.0
    w_max: float = DEFAULT_STDP_MAX_WEIGHT

    def _array(self):
        return (ctypes.c_double * 6)(self.a_plus, self.a_minus, self.tau_plus, self.tau_minus, self.w_min, self.w_max)


def _doubles(values: Sequence[float]):
    arr = (ctypes.c_double * len(values))(*[float(v) for v in values])
    return arr


class Population:
    def __init__(self, network: "Network", index: int, name: str):
        self._net = network
        self.index = index
        self.name = name

    @property
    def size(self) -> int:
        return _check(lib().gygax_network_population_size(self._net._h, self.index))

    def set_bias(self, current: float) -> None:
        _check(lib().gygax_network_set_bias(self._net._h, self.index, float(current)))

    def set_drive(self, currents: Sequence[float]) -> None:
        arr = _doubles(currents)
        _check(lib().gygax_network_set_drive(self._net._h, self.index, arr, len(arr)))

    def set_rate(self, rate_hz: float) -> None:
        _check(lib().gygax_network_set_rate(self._net._h, self.index, float(rate_hz)))

    def set_rates(self, rates_hz: Sequence[float]) -> None:
        arr = _doubles(rates_hz)
        _check(lib().gygax_network_set_rates(self._net._h, self.index, arr, len(arr)))

    def inject_spike(self, neuron: int) -> None:
        _check(lib().gygax_network_inject_spike(self._net._h, self.index, int(neuron)))

    def record_voltage(self, enabled: bool = True) -> None:
        _check(lib().gygax_network_set_record_voltage(self._net._h, self.index, int(enabled)))

    @property
    def spike_count(self) -> int:
        return _check(lib().gygax_network_spike_count(self._net._h, self.index))

    @property
    def mean_rate_hz(self) -> float:
        value = lib().gygax_network_mean_rate_hz(self._net._h, self.index)
        if value < 0:
            raise GygaxError(lib().gygax_last_error().decode())
        return value

    def spike_counts(self):
        n = self.size
        buf = (ctypes.c_uint32 * n)()
        _check(lib().gygax_network_spike_counts(self._net._h, self.index, buf, n))
        return _np.frombuffer(buf, dtype=_np.uint32).copy() if _np is not None else list(buf)

    def spikes(self):
        total = _check(lib().gygax_network_spikes(self._net._h, self.index, None, None, 0))
        times = (ctypes.c_double * total)()
        neurons = (ctypes.c_uint32 * total)()
        _check(lib().gygax_network_spikes(self._net._h, self.index, times, neurons, total))
        if _np is not None:
            return _np.frombuffer(times, dtype=_np.float64).copy(), _np.frombuffer(neurons, dtype=_np.uint32).copy()
        return list(times), list(neurons)

    def membrane_potentials(self):
        n = self.size
        buf = (ctypes.c_double * n)()
        _check(lib().gygax_network_membrane(self._net._h, self.index, buf, n))
        return _np.frombuffer(buf, dtype=_np.float64).copy() if _np is not None else list(buf)

    def voltage_trace(self):
        total = _check(lib().gygax_network_voltage_trace(self._net._h, self.index, None, 0))
        buf = (ctypes.c_double * total)()
        _check(lib().gygax_network_voltage_trace(self._net._h, self.index, buf, total))
        return _np.frombuffer(buf, dtype=_np.float64).copy() if _np is not None else list(buf)

    def __repr__(self) -> str:
        return f"Population({self.name!r}, index={self.index})"


class Projection:
    def __init__(self, network: "Network", index: int):
        self._net = network
        self.index = index

    @property
    def synapse_count(self) -> int:
        return _check(lib().gygax_network_synapse_count(self._net._h, self.index))

    @property
    def mean_weight(self) -> float:
        return lib().gygax_network_mean_weight(self._net._h, self.index)

    def weights(self):
        n = self.synapse_count
        buf = (ctypes.c_float * n)()
        _check(lib().gygax_network_weights(self._net._h, self.index, buf, n))
        return _np.frombuffer(buf, dtype=_np.float32).copy() if _np is not None else list(buf)


class Network:
    def __init__(self, dt_ms: float = DEFAULT_NETWORK_TIMESTEP_MS, seed: int = DEFAULT_RANDOM_SEED, _handle=None):
        if _handle is None:
            _handle = lib().gygax_network_create(float(dt_ms), int(seed))
            if not _handle:
                raise GygaxError(lib().gygax_last_error().decode())
        self._h = _handle
        self._pops: Dict[str, Population] = {}

    @classmethod
    def from_spec(cls, spec: dict) -> "Network":
        handle = lib().gygax_network_from_json(json.dumps(spec).encode())
        if not handle:
            raise GygaxError(lib().gygax_last_error().decode())
        net = cls(_handle=handle)
        for i in range(lib().gygax_network_population_count(handle)):
            pass
        for p in spec.get("populations", []):
            idx = lib().gygax_network_find_population(handle, p["name"].encode())
            net._pops[p["name"]] = Population(net, idx, p["name"])
        return net

    def close(self) -> None:
        if getattr(self, "_h", None):
            lib().gygax_network_destroy(self._h)
            self._h = None

    def __enter__(self) -> "Network":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def _register(self, name: str, index: int) -> Population:
        pop = Population(self, _check(index), name)
        self._pops[name] = pop
        return pop

    def population(self, name: str) -> Population:
        return self._pops[name]

    def add_lif(self, name: str, n: int, params: Optional[LifParams] = None) -> Population:
        arr = (params or LifParams())._array()
        return self._register(name, lib().gygax_network_add_lif(self._h, name.encode(), n, arr))

    def add_izhikevich(self, name: str, n: int, params: Optional[IzhikevichParams] = None) -> Population:
        arr = (params or IzhikevichParams())._array()
        return self._register(name, lib().gygax_network_add_izhikevich(self._h, name.encode(), n, arr))

    def add_poisson(self, name: str, n: int, rate_hz: float) -> Population:
        return self._register(name, lib().gygax_network_add_poisson(self._h, name.encode(), n, float(rate_hz)))

    def add_spike_source(self, name: str, n: int) -> Population:
        return self._register(name, lib().gygax_network_add_spike_source(self._h, name.encode(), n))

    def connect(
        self,
        pre: Population,
        post: Population,
        kind: int = FIXED_PROBABILITY,
        p: float = DEFAULT_CONNECTION_PROBABILITY,
        weight: float = DEFAULT_CONNECTION_WEIGHT,
        weight_std: float = 0.0,
        delay_ms: float = DEFAULT_CONNECTION_DELAY_MS,
        allow_self: bool = False,
        stdp: Optional[Stdp] = None,
    ) -> Projection:
        arr = stdp._array() if stdp else None
        idx = lib().gygax_network_connect(
            self._h, pre.index, post.index, kind, p, weight, weight_std, delay_ms, int(allow_self), arr
        )
        return Projection(self, _check(idx))

    def connect_explicit(
        self,
        pre: Population,
        post: Population,
        pre_idx: Sequence[int],
        post_idx: Sequence[int],
        weights: Sequence[float],
        delay_ms: float = DEFAULT_CONNECTION_DELAY_MS,
        stdp: Optional[Stdp] = None,
    ) -> Projection:
        n = len(weights)
        a = (ctypes.c_uint32 * n)(*[int(v) for v in pre_idx])
        b = (ctypes.c_uint32 * n)(*[int(v) for v in post_idx])
        w = (ctypes.c_float * n)(*[float(v) for v in weights])
        arr = stdp._array() if stdp else None
        idx = lib().gygax_network_connect_explicit(self._h, pre.index, post.index, n, a, b, w, delay_ms, arr)
        return Projection(self, _check(idx))

    def set_plasticity(self, enabled: bool) -> None:
        _check(lib().gygax_network_set_plasticity(self._h, int(enabled)))

    def run(self, duration_ms: float) -> None:
        _check(lib().gygax_network_run(self._h, float(duration_ms)))

    def step(self) -> None:
        _check(lib().gygax_network_step(self._h))

    def reset(self, restore_weights: bool = True) -> None:
        _check(lib().gygax_network_reset(self._h, int(restore_weights)))

    def clear_spikes(self) -> None:
        _check(lib().gygax_network_clear_spikes(self._h))

    @property
    def time_ms(self) -> float:
        return lib().gygax_network_time_ms(self._h)

    def describe(self) -> dict:
        return json.loads(_take_string(lib().gygax_network_describe(self._h)))


def simulate(spec: dict) -> dict:
    return json.loads(_take_string(lib().gygax_simulate_json(json.dumps(spec).encode())))


def rate_encode(values: Iterable[float], max_rate_hz: float = DEFAULT_MAX_RATE_HZ) -> List[float]:
    return [min(max(float(v), 0.0), 1.0) * max_rate_hz for v in values]
