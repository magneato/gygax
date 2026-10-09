"""SatLink: SGP4/SDP4 satellite tracking, Doppler, Hamlib radio control and rtl_tcp SDR.

    from gygax import satlink

    iss = satlink.Satellite(line1, line2, "ISS (ZARYA)")
    home = satlink.Observer(51.4779, -0.0015, 46)
    for p in iss.passes(home, satlink.timescale.now(), satlink.timescale.now().unix + 86400, 10):
        print(p.rise.utc_iso(), "%.0f°" % p.max_elevation_deg)

The orbit work runs in the native library (loaded on first use); radio and SDR control are pure Python.
"""

from .predict import (
    SPEED_OF_LIGHT_KM_S,
    Angle,
    Distance,
    EarthSatellite,
    Look,
    Observer,
    Pass,
    SatLinkError,
    Satellite,
    Subpoint,
    Time,
    Timescale,
    doppler_shift_hz,
    elevation_and_distance,
    load,
    observer_at,
    read_tles,
    satellite_from_tle,
    timescale,
    wgs84,
)
from .rig import Rig, RigError, set_rig_frequency
from .sdr import RtlTcp, SdrError

__all__ = [
    "Angle", "Distance", "EarthSatellite", "Look", "Observer", "Pass", "Rig", "RigError", "RtlTcp", "SdrError", "SPEED_OF_LIGHT_KM_S",
    "SatLinkError", "Satellite", "Subpoint", "Time", "Timescale", "doppler_shift_hz", "elevation_and_distance", "load",
    "observer_at", "read_tles", "satellite_from_tle", "set_rig_frequency", "timescale", "wgs84",
]
