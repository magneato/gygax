import ctypes
import os
import sys
from pathlib import Path

_LIB_NAMES = {
    "linux": ["libgygax_c.so"],
    "darwin": ["libgygax_c.dylib"],
    "win32": ["gygax_c.dll"],
}


def _candidates():
    env = os.environ.get("GYGAX_LIB")
    if env:
        yield Path(env)
    names = _LIB_NAMES.get(sys.platform, _LIB_NAMES["linux"])
    here = Path(__file__).resolve().parent
    roots = [here, here.parent, here.parent.parent]
    for root in roots:
        for name in names:
            yield root / name
        for sub in ("build", "build/dev", "build/linux-clang", "build/dogfood", "lib"):
            for name in names:
                yield root / sub / name
    installed = here.parent.parent.parent.parent / "lib"
    if installed.is_dir():
        for name in names:
            yield installed / name
            for found in sorted(installed.glob("*/" + name)):
                yield found
    prefix = os.environ.get("GYGAX_PREFIX")
    if prefix:
        for name in names:
            yield Path(prefix) / "lib" / name
    for name in names:
        yield Path(name)


def load():
    tried = []
    for candidate in _candidates():
        tried.append(str(candidate))
        try:
            if candidate.is_absolute() and not candidate.exists():
                continue
            return ctypes.CDLL(str(candidate))
        except OSError:
            continue
    raise ImportError(
        "could not load the gygax native library; build it with `cmake --build build --target gygax_c` "
        "and set GYGAX_LIB to its path. Searched: " + ", ".join(tried[:8]) + " ..."
    )


_lib = None


def lib():
    global _lib
    if _lib is None:
        _lib = _bind(load())
    return _lib


def _bind(L):
    c = ctypes
    dbl_p = c.POINTER(c.c_double)
    f32_p = c.POINTER(c.c_float)
    u32_p = c.POINTER(c.c_uint32)
    net = c.c_void_p

    def sig(name, restype, *argtypes):
        fn = getattr(L, name)
        fn.restype = restype
        fn.argtypes = list(argtypes)

    sig("gygax_version", c.c_char_p)
    sig("gygax_last_error", c.c_char_p)
    sig("gygax_free", None, c.c_void_p)
    sig("gygax_network_create", net, c.c_double, c.c_uint64)
    sig("gygax_network_from_json", net, c.c_char_p)
    sig("gygax_network_destroy", None, net)
    sig("gygax_network_add_lif", c.c_int, net, c.c_char_p, c.c_size_t, dbl_p)
    sig("gygax_network_add_izhikevich", c.c_int, net, c.c_char_p, c.c_size_t, dbl_p)
    sig("gygax_network_add_poisson", c.c_int, net, c.c_char_p, c.c_size_t, c.c_double)
    sig("gygax_network_add_spike_source", c.c_int, net, c.c_char_p, c.c_size_t)
    sig("gygax_network_connect", c.c_int, net, c.c_int, c.c_int, c.c_int, c.c_double, c.c_double, c.c_double, c.c_double, c.c_int, dbl_p)
    sig("gygax_network_connect_explicit", c.c_int, net, c.c_int, c.c_int, c.c_size_t, u32_p, u32_p, f32_p, c.c_double, dbl_p)
    sig("gygax_network_set_bias", c.c_int, net, c.c_int, c.c_double)
    sig("gygax_network_set_drive", c.c_int, net, c.c_int, dbl_p, c.c_size_t)
    sig("gygax_network_set_rate", c.c_int, net, c.c_int, c.c_double)
    sig("gygax_network_set_rates", c.c_int, net, c.c_int, dbl_p, c.c_size_t)
    sig("gygax_network_inject_spike", c.c_int, net, c.c_int, c.c_size_t)
    sig("gygax_network_set_plasticity", c.c_int, net, c.c_int)
    sig("gygax_network_set_record_voltage", c.c_int, net, c.c_int, c.c_int)
    sig("gygax_network_run", c.c_int, net, c.c_double)
    sig("gygax_network_step", c.c_int, net)
    sig("gygax_network_reset", c.c_int, net, c.c_int)
    sig("gygax_network_clear_spikes", c.c_int, net)
    sig("gygax_network_time_ms", c.c_double, net)
    sig("gygax_network_population_count", c.c_int, net)
    sig("gygax_network_projection_count", c.c_int, net)
    sig("gygax_network_find_population", c.c_int, net, c.c_char_p)
    sig("gygax_network_population_size", c.c_longlong, net, c.c_int)
    sig("gygax_network_spike_count", c.c_longlong, net, c.c_int)
    sig("gygax_network_mean_rate_hz", c.c_double, net, c.c_int)
    sig("gygax_network_synapse_count", c.c_longlong, net, c.c_int)
    sig("gygax_network_mean_weight", c.c_double, net, c.c_int)
    sig("gygax_network_spike_counts", c.c_longlong, net, c.c_int, u32_p, c.c_size_t)
    sig("gygax_network_spikes", c.c_longlong, net, c.c_int, dbl_p, u32_p, c.c_size_t)
    sig("gygax_network_weights", c.c_longlong, net, c.c_int, f32_p, c.c_size_t)
    sig("gygax_network_membrane", c.c_longlong, net, c.c_int, dbl_p, c.c_size_t)
    sig("gygax_network_voltage_trace", c.c_longlong, net, c.c_int, dbl_p, c.c_size_t)
    sig("gygax_network_describe", c.c_void_p, net)
    sig("gygax_simulate_json", c.c_void_p, c.c_char_p)
    vec = c.c_void_p
    u8_p = c.POINTER(c.c_uint8)
    sig("gygax_vecenv_create", vec, c.c_char_p)
    sig("gygax_vecenv_destroy", None, vec)
    sig("gygax_vecenv_num_envs", c.c_longlong, vec)
    sig("gygax_vecenv_observation_size", c.c_longlong, vec)
    sig("gygax_vecenv_action_size", c.c_longlong, vec)
    sig("gygax_vecenv_reset", c.c_int, vec, c.c_uint64)
    sig("gygax_vecenv_step", c.c_int, vec, f32_p, c.c_size_t)
    for name in ("observations", "final_observations", "rewards"):
        sig("gygax_vecenv_" + name, f32_p, vec)
    for name in ("terminated", "truncated", "collided", "reached_goal"):
        sig("gygax_vecenv_" + name, u8_p, vec)
    sig("gygax_lif_rate_hz", c.c_double, *([c.c_double] * 7))

    # SatLink (structs are passed by pointer, so c_void_p keeps the signatures simple)
    sat = c.c_void_p
    ptr = c.c_void_p
    sig("gygax_satlink_create", sat, c.c_char_p, c.c_char_p, c.c_char_p)
    sig("gygax_satlink_destroy", None, sat)
    sig("gygax_satlink_epoch_unix", c.c_double, sat)
    sig("gygax_satlink_period_minutes", c.c_double, sat)
    sig("gygax_satlink_is_deep_space", c.c_int, sat)
    sig("gygax_satlink_state", c.c_int, sat, c.c_double, c.c_int, dbl_p, dbl_p)
    sig("gygax_satlink_subpoint", c.c_int, sat, c.c_double, dbl_p, dbl_p, dbl_p)
    sig("gygax_satlink_observe", c.c_int, sat, c.c_double, c.c_double, c.c_double, c.c_double, ptr)
    sig("gygax_satlink_doppler_hz", c.c_double, sat, *([c.c_double] * 5))
    sig("gygax_satlink_passes", c.c_longlong, sat, *([c.c_double] * 6), ptr, c.c_size_t)
    sig("gygax_satlink_rig_set_freq", c.c_void_p, c.c_char_p, c.c_int, c.c_double, c.c_int)

    return L
