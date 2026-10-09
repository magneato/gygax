"""SatLink prediction: SGP4/SDP4 orbits, look angles, Doppler and passes, computed in C++.

Two ways in:

* The SatLink API: ``Satellite``, ``Observer``, ``Satellite.observe``, ``Satellite.passes``,
  ``doppler_shift_hz``.
* A Skyfield-shaped subset for scripts written against Skyfield: ``load.timescale()``,
  ``EarthSatellite(line1, line2, name, ts)``, ``wgs84.latlon(...)``,
  ``(satellite - observer).at(t).altaz()`` and ``satellite.find_events(...)``.

Times are UTC. Anything accepted as a time may be a ``Time``, a ``datetime`` (naive means UTC) or
Unix seconds.
"""

import ctypes
import math
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import List, Optional, Sequence, Tuple, Union

from .._native import lib

SPEED_OF_LIGHT_KM_S = 299792.458

_TEME = 0
_ECEF = 1
_DEFAULT_PASS_CAPACITY = 64


class SatLinkError(RuntimeError):
    """Raised when the native library rejects elements or cannot propagate them."""


def _last_error() -> str:
    message = lib().gygax_last_error()
    return message.decode("utf-8", "replace") if message else "unknown error"


# --- time -------------------------------------------------------------------------------------


class Time:
    """A UTC instant (Skyfield's ``Time``, reduced to what tracking needs)."""

    __slots__ = ("unix",)

    def __init__(self, unix_seconds: float):
        self.unix = float(unix_seconds)

    def utc_datetime(self) -> datetime:
        return datetime.fromtimestamp(self.unix, tz=timezone.utc)

    def utc_iso(self, places: int = 0) -> str:
        dt = self.utc_datetime()
        text = dt.strftime("%Y-%m-%dT%H:%M:%S")
        if places > 0:
            text += ("%.*f" % (places, dt.microsecond / 1e6))[1:]
        return text + "Z"

    def __sub__(self, other: "Time") -> float:
        """Difference in days, as Skyfield returns it."""
        return (self.unix - other.unix) / 86400.0

    def __lt__(self, other: "Time") -> bool:
        return self.unix < other.unix

    def __eq__(self, other: object) -> bool:
        return isinstance(other, Time) and self.unix == other.unix

    def __hash__(self) -> int:
        return hash(self.unix)

    def __repr__(self) -> str:
        return "<Time %s>" % self.utc_iso(3)


TimeLike = Union[Time, datetime, float, int]


def _unix(t: TimeLike) -> float:
    if isinstance(t, Time):
        return t.unix
    if isinstance(t, datetime):
        if t.tzinfo is None:
            t = t.replace(tzinfo=timezone.utc)
        return t.timestamp()
    return float(t)


class Timescale:
    """Builds ``Time`` values (Skyfield's ``load.timescale()``)."""

    def now(self) -> Time:
        return Time(datetime.now(timezone.utc).timestamp())

    def from_datetime(self, dt: datetime) -> Time:
        return Time(_unix(dt))

    def utc(self, year: int, month: int = 1, day: int = 1, hour: int = 0, minute: int = 0, second: float = 0.0) -> Time:
        whole = int(math.floor(second))
        base = datetime(year, month, day, hour, minute, whole, tzinfo=timezone.utc).timestamp()
        return Time(base + (second - whole))

    def from_unix(self, seconds: float) -> Time:
        return Time(seconds)


timescale = Timescale()


class _Loader:
    @staticmethod
    def timescale() -> Timescale:
        return timescale


load = _Loader()


# --- places and readings ----------------------------------------------------------------------


class Angle:
    __slots__ = ("degrees",)

    def __init__(self, degrees: float):
        self.degrees = float(degrees)

    @property
    def radians(self) -> float:
        return math.radians(self.degrees)

    def __float__(self) -> float:
        return self.degrees

    def __repr__(self) -> str:
        return "<Angle %.4f°>" % self.degrees


class Distance:
    __slots__ = ("km",)

    def __init__(self, km: float):
        self.km = float(km)

    @property
    def m(self) -> float:
        return self.km * 1000.0

    def __float__(self) -> float:
        return self.km

    def __repr__(self) -> str:
        return "<Distance %.3f km>" % self.km


@dataclass(frozen=True)
class Observer:
    """A ground station on the WGS-84 ellipsoid."""

    latitude_deg: float
    longitude_deg: float
    elevation_m: float = 0.0

    # Skyfield-style accessors.
    @property
    def latitude(self) -> Angle:
        return Angle(self.latitude_deg)

    @property
    def longitude(self) -> Angle:
        return Angle(self.longitude_deg)


@dataclass(frozen=True)
class Look:
    """What a station sees at one instant."""

    elevation_deg: float
    azimuth_deg: float
    range_km: float
    range_rate_km_s: float  # positive while receding

    def doppler_shift_hz(self, carrier_hz: float) -> float:
        return -carrier_hz * self.range_rate_km_s / SPEED_OF_LIGHT_KM_S

    # Skyfield's altaz(): (altitude, azimuth, distance).
    def altaz(self) -> Tuple[Angle, Angle, Distance]:
        return Angle(self.elevation_deg), Angle(self.azimuth_deg), Distance(self.range_km)


@dataclass(frozen=True)
class Pass:
    rise: Time
    culmination: Time
    set: Time
    max_elevation_deg: float
    rise_azimuth_deg: float
    set_azimuth_deg: float
    rise_clipped: bool = False  # already up when the search window opened
    set_clipped: bool = False  # still up when it closed

    @property
    def duration_s(self) -> float:
        return self.set.unix - self.rise.unix


@dataclass(frozen=True)
class Subpoint:
    latitude_deg: float
    longitude_deg: float
    altitude_km: float


class _Look(ctypes.Structure):
    _fields_ = [("elevation_deg", ctypes.c_double), ("azimuth_deg", ctypes.c_double),
                ("range_km", ctypes.c_double), ("range_rate_km_s", ctypes.c_double)]


class _Pass(ctypes.Structure):
    _fields_ = [("rise_unix", ctypes.c_double), ("culmination_unix", ctypes.c_double), ("set_unix", ctypes.c_double),
                ("max_elevation_deg", ctypes.c_double), ("rise_azimuth_deg", ctypes.c_double),
                ("set_azimuth_deg", ctypes.c_double), ("rise_clipped", ctypes.c_int), ("set_clipped", ctypes.c_int)]


# --- satellites -------------------------------------------------------------------------------


class Satellite:
    """A two-line element set and its SGP4/SDP4 propagator (checksums are verified)."""

    def __init__(self, line1: str, line2: str, name: Optional[str] = None):
        name = (name or "").strip()
        self.name = name[2:] if name.startswith("0 ") else name
        self.line1 = line1
        self.line2 = line2
        L = lib()
        handle = L.gygax_satlink_create(self.name.encode(), line1.encode(), line2.encode())
        if not handle:
            raise SatLinkError(_last_error())
        self._handle = ctypes.c_void_p(handle)
        self.epoch = Time(L.gygax_satlink_epoch_unix(self._handle))
        self.period_minutes = L.gygax_satlink_period_minutes(self._handle)
        self.deep_space = bool(L.gygax_satlink_is_deep_space(self._handle))

    def close(self) -> None:
        handle, self._handle = getattr(self, "_handle", None), None
        if handle:
            lib().gygax_satlink_destroy(handle)

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:  # interpreter shutdown
            pass

    def __enter__(self) -> "Satellite":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __repr__(self) -> str:
        return "<Satellite %r epoch %s>" % (self.name or self.line1[2:7], self.epoch.utc_iso())

    def _state(self, t: TimeLike, frame: int) -> Tuple[Tuple[float, float, float], Tuple[float, float, float]]:
        r = (ctypes.c_double * 3)()
        v = (ctypes.c_double * 3)()
        if lib().gygax_satlink_state(self._handle, _unix(t), frame, r, v) != 0:
            raise SatLinkError(_last_error())
        return (r[0], r[1], r[2]), (v[0], v[1], v[2])

    def teme(self, t: TimeLike):
        """TEME position (km) and velocity (km/s), straight from SGP4."""
        return self._state(t, _TEME)

    def ecef(self, t: TimeLike):
        """Earth-fixed position (km) and velocity (km/s)."""
        return self._state(t, _ECEF)

    def subpoint(self, t: TimeLike) -> Subpoint:
        lat, lon, alt = ctypes.c_double(), ctypes.c_double(), ctypes.c_double()
        if lib().gygax_satlink_subpoint(self._handle, _unix(t), ctypes.byref(lat), ctypes.byref(lon), ctypes.byref(alt)) != 0:
            raise SatLinkError(_last_error())
        return Subpoint(lat.value, lon.value, alt.value)

    def observe(self, observer: Observer, t: TimeLike) -> Look:
        out = _Look()
        if lib().gygax_satlink_observe(self._handle, observer.latitude_deg, observer.longitude_deg,
                                       observer.elevation_m, _unix(t), ctypes.byref(out)) != 0:
            raise SatLinkError(_last_error())
        return Look(out.elevation_deg, out.azimuth_deg, out.range_km, out.range_rate_km_s)

    def doppler_shift_hz(self, observer: Observer, t: TimeLike, carrier_hz: float) -> float:
        shift = lib().gygax_satlink_doppler_hz(self._handle, observer.latitude_deg, observer.longitude_deg,
                                               observer.elevation_m, _unix(t), float(carrier_hz))
        if math.isnan(shift):
            raise SatLinkError(_last_error())
        return shift

    def passes(self, observer: Observer, start: TimeLike, end: TimeLike, min_elevation_deg: float = 0.0) -> List[Pass]:
        """Passes above ``min_elevation_deg`` between two times, earliest first."""
        L = lib()
        args = (self._handle, observer.latitude_deg, observer.longitude_deg, observer.elevation_m,
                _unix(start), _unix(end), float(min_elevation_deg))
        capacity = _DEFAULT_PASS_CAPACITY
        while True:
            buffer = (_Pass * capacity)()
            total = L.gygax_satlink_passes(*args, buffer, capacity)
            if total < 0:
                raise SatLinkError(_last_error())
            if total <= capacity:
                break
            capacity = total
        return [Pass(Time(p.rise_unix), Time(p.culmination_unix), Time(p.set_unix), p.max_elevation_deg,
                     p.rise_azimuth_deg, p.set_azimuth_deg, bool(p.rise_clipped), bool(p.set_clipped))
                for p in buffer[:total]]

    # --- Skyfield-shaped surface ---

    def __sub__(self, observer: Observer) -> "_Relative":
        return _Relative(self, observer)

    def find_events(self, observer: Observer, t0: TimeLike, t1: TimeLike, altitude_degrees: float = 0.0):
        """Skyfield's ``find_events``: (times, events) with 0 = rise, 1 = culminate, 2 = set.

        Like Skyfield, a pass already in progress at ``t0`` contributes no rise and one still in
        progress at ``t1`` no set.
        """
        times: List[Time] = []
        events: List[int] = []
        for p in self.passes(observer, t0, t1, altitude_degrees):
            if not p.rise_clipped:
                times.append(p.rise)
                events.append(0)
            times.append(p.culmination)
            events.append(1)
            if not p.set_clipped:
                times.append(p.set)
                events.append(2)
        return times, events


class _Relative:
    """``satellite - observer``, evaluated with ``.at(t)``."""

    def __init__(self, satellite: Satellite, observer: Observer):
        self._satellite = satellite
        self._observer = observer

    def at(self, t: TimeLike) -> Look:
        return self._satellite.observe(self._observer, t)


def EarthSatellite(line1: str, line2: str, name: Optional[str] = None, ts: Optional[Timescale] = None) -> Satellite:
    """Skyfield's constructor order: lines first, then the name."""
    return Satellite(line1, line2, name)


class _Wgs84:
    @staticmethod
    def latlon(latitude_degrees: float, longitude_degrees: float, elevation_m: float = 0.0) -> Observer:
        return Observer(float(latitude_degrees), float(longitude_degrees), float(elevation_m))


wgs84 = _Wgs84()


# --- functional helpers -------------------------------------------------------------------------


def satellite_from_tle(name: str, line1: str, line2: str) -> Satellite:
    return Satellite(line1, line2, name)


def observer_at(latitude: float, longitude: float, elevation_m: float = 0.0) -> Observer:
    return Observer(float(latitude), float(longitude), float(elevation_m))


def elevation_and_distance(satellite: Satellite, observer: Observer, when: TimeLike) -> Tuple[float, float]:
    """Elevation (degrees) and slant range (km)."""
    look = satellite.observe(observer, when)
    return look.elevation_deg, look.range_km


def doppler_shift_hz(satellite: Satellite, observer: Observer, when: TimeLike, carrier_hz: float) -> float:
    """Shift of a downlink carrier as received: add it to tune a receiver."""
    return satellite.doppler_shift_hz(observer, when, carrier_hz)


def read_tles(text: str) -> List[Satellite]:
    """Every element set in a TLE file's text (two- or three-line format)."""
    satellites: List[Satellite] = []
    name: Optional[str] = None
    line1: Optional[str] = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("1 ") and len(line) >= 64:
            line1 = line
        elif line.startswith("2 ") and line1:
            satellites.append(Satellite(line1, line, name))
            name, line1 = None, None
        else:
            name, line1 = line.strip(), None
    return satellites


__all__: Sequence[str] = [
    "Angle", "Distance", "EarthSatellite", "Look", "Observer", "Pass", "SPEED_OF_LIGHT_KM_S", "SatLinkError",
    "Satellite", "Subpoint", "Time", "Timescale", "doppler_shift_hz", "elevation_and_distance", "load", "observer_at",
    "read_tles", "satellite_from_tle", "timescale", "wgs84",
]
