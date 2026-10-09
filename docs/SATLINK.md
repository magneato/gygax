# SatLink: satellite tracking

SatLink predicts where a satellite is and how to work it from the ground: SGP4/SDP4 propagation of NORAD two-line elements, look angles from a station, the exact range rate and the Doppler shift it causes, rise/culminate/set pass prediction, radio retuning through Hamlib `rigctld`, and reception with a software-defined radio over `rtl_tcp`. It is C++ (`include/gygax/satlink/satlink.hpp`, `sdr.hpp`), exposed through the C ABI (`gygax_satlink_*`), Python (`gygax.satlink`) and the CLI (`gygax track`).

## From the command line

```
$ gygax track --tle examples/satlink/sample.tle --at 51.4779,-0.0015,46 --start 2024-01-01T12:00 --downlink 437.8e6
ISS (ZARYA)  ·  NORAD 25544  ·  epoch 2024-01-01 12:00:00 UTC  ·  period 92.9 min  ·  SGP4
station 51.4779°N 0.0015°W 46 m

4 passes above 10° in the 24 hours from 2024-01-01 12:00:00 UTC

  rise (UTC)            from       peak (UTC)  max el   set (UTC)   to         lasts    doppler rise → set
  2024-01-01 13:22:56   219.5° SW   13:26:02     35.2°  13:29:08    84.7° E     6m11s    +9.21 kHz → -9.22 kHz
  2024-01-01 14:59:15   260.2° W    15:02:37     87.7°  15:05:58    82.8° E     6m43s    +9.93 kHz → -9.93 kHz
  2024-01-01 16:36:03   278.5° W    16:39:24     81.4°  16:42:44   104.6° ESE   6m41s    +9.93 kHz → -9.93 kHz
  2024-01-01 18:12:58   272.6° W    18:15:54     28.9°  18:18:50   149.0° SSE   5m53s    +8.81 kHz → -8.79 kHz
```

`--live` prints azimuth, elevation, range, range rate and the corrected receive frequency once a second; with `--rig HOST[:PORT]` it also retunes the radio every second. `--sat` picks a satellite from a file of many (name substring or catalog number), `--min-elevation` sets the mask (default 10°), `--hours` the window. The command warns when the elements are more than two weeks from epoch.

Element sets age: fetch current ones before tracking, for example `curl -s 'https://celestrak.org/NORAD/elements/gp.php?GROUP=amateur&FORMAT=tle' > amateur.tle`. Gygax itself never fetches anything.

## From Python

```python
from gygax import satlink

iss = satlink.Satellite(line1, line2, "ISS (ZARYA)")      # checksums are verified
home = satlink.Observer(51.4779, -0.0015, 46)             # degrees, degrees, metres
ts = satlink.load.timescale()

for p in iss.passes(home, ts.now(), ts.now().unix + 86400, min_elevation_deg=10):
    print(p.rise.utc_iso(), "peak %.1f°" % p.max_elevation_deg, "%.0f s" % p.duration_s)

look = iss.observe(home, ts.now())                        # elevation, azimuth, range, range rate
tune = 437.8e6 + look.doppler_shift_hz(437.8e6)

with satlink.Rig("127.0.0.1", 4532) as rig:               # Hamlib rigctld, one connection
    rig.set_frequency(tune)                                # raises RigError unless "RPRT 0"
```

Times may be `satlink.Time`, a `datetime` (naive means UTC) or Unix seconds. `teme(t)` and `ecef(t)` return position (km) and velocity (km/s); `subpoint(t)` the ground point under the satellite; `read_tles(text)` every set in a two- or three-line file. `examples/satlink/track.py` is a complete pass planner that can follow the next pass with a radio.

**Skyfield-shaped subset.** Scripts written against Skyfield's satellite API run unchanged for these calls: `load.timescale()`, `ts.now()`, `ts.utc(...)`, `ts.from_datetime(...)`, `EarthSatellite(line1, line2, name, ts)`, `wgs84.latlon(lat, lon, elevation_m=...)`, `(satellite - observer).at(t).altaz()` and `satellite.find_events(observer, t0, t1, altitude_degrees=...)` (with 0 = rise, 1 = culminate, 2 = set, returned as lists rather than arrays). The rest of Skyfield (planets, other frames, `Time` arrays) is not provided. `import satlink` is an alias for `gygax.satlink`.

The orbit work needs the native library (`libgygax_c`, staged by the build in `build/python`); there is no pure-Python fallback that guesses. Radio control is pure Python.

## From C++ and C

```cpp
#include <gygax/satlink/satlink.hpp>
using namespace gygax::satlink;

const Satellite iss("ISS (ZARYA)", line1, line2);          // throws TleError naming line and columns
const Observer home{51.4779, -0.0015, 46.0};
const Topocentric look = iss.observe(home, unix_seconds);   // throws Sgp4Error (e.g. decayed)
const double shift = iss.dopplerShiftHz(home, unix_seconds, 437.8e6);
for (const Pass& p : iss.passes(home, start, start + 86400, 10.0)) { ... }

RigClient rig("127.0.0.1", 4532);
rig.setFrequency(437.8e6 + shift);
```

`Tle` holds every field of the element set in its printed units, `Sgp4` is the bare propagator (minutes from epoch in, TEME state out, thread-safe and pure), and the free functions `temeToEcef`, `ecefToGeodetic`, `lookAngles`, `greenwichSiderealRad` and `dopplerShiftHz` are available on their own. The C interface in `gygax/capi/gygax.h` mirrors this with an opaque `gygax_satellite`, the `gygax_satlink_look` and `gygax_satlink_pass` structs, NaN or -1 for errors, and `gygax_last_error()` for the reason.

## How it works

**Elements.** `Tle::parse` reads the fixed columns of both lines, accepts a title line (with or without the `0 ` prefix) and Alpha-5 catalog numbers, verifies the checksum in column 69 (digits summed, each `-` counting one, modulo 10), and checks that both lines carry the same catalog number. Any failure names the line, the columns and the field.

**Propagation.** SGP4 for orbits under 225 minutes and SDP4, with lunar-solar perturbations and 12- and 24-hour resonance integration, above that. The code follows Vallado, Crawford, Hujsak and Kelso, *Revisiting Spacetrack Report #3* (AIAA 2006-6753), routine by routine with the paper's variable names, in its improved operation mode with WGS-72 constants (the constants TLEs are fitted with; WGS-84 is selectable). Unlike the reference code, the deep-space integrator restarts from epoch on every call instead of caching its last step: it reaches the same 720-minute grid and the same answer, and propagation stays a pure, thread-safe function of time.

**Frames.** SGP4 produces TEME. Earth-fixed coordinates are TEME rotated by Greenwich mean sidereal time (IAU-82, the convention SGP4 itself uses), and the velocity loses the frame rotation `ω × r`. Sidereal time is computed from Unix seconds directly: going through a Julian date, whose doubles near 2.46 million resolve only 40 µs, would jitter the Earth-fixed position by about 2 cm. Geodetic latitude and height use the WGS-84 ellipsoid (iterated to convergence; exact to 1e-9° and 0.01 mm everywhere from the surface to geostationary height).

**Look angles and Doppler.** With `ρ` the vector from station to satellite in Earth-fixed coordinates, elevation and azimuth come from its east, north and up components, and the range rate is the satellite's Earth-fixed velocity projected on `ρ̂` (the station is at rest in that frame). The received frequency of a carrier `f` is shifted by

```
Δf = −f · ṙ / c        (ṙ > 0 receding, c = 299 792.458 km/s)
```

so a satellite rises high and sets low. This is first order in `ṙ/c`; the neglected relativistic terms stay under 0.3 Hz at 437 MHz. For a downlink, tune the receiver to `f + Δf`; for an uplink, transmit `f − Δf` so the satellite hears `f`. For the ISS at 437.8 MHz the shift spans about ±10 kHz on an overhead pass.

**Passes.** The window is scanned every 20 seconds (or 1/200 of an orbit, if shorter) for changes of sign in elevation minus the mask; each crossing is bisected to 10 ms, and the peak is refined by golden-section search to 0.1 s within a step of the highest scanned sample, which holds for any pass length (a geostationary satellite is "up" all day with no single hump). A pass already in progress when the window opens, or still in progress when it closes, is marked as clipped. A pass shorter than one scan step can be missed.

**Radio.** `RigClient` speaks Hamlib's `rigctld` protocol over TCP (default port 4532): `F <hz>` answered by `RPRT 0`, and `f` answered by the frequency. It connects on first use, keeps the connection for later commands, and retries once on a fresh connection if `rigctld` has dropped an idle one. Connect and reply timeouts default to 2 s. Test it without a radio with `rigctld -m 1` (Hamlib's dummy rig).

## With a software-defined radio

`rtl_tcp` serves an RTL-SDR dongle over TCP (`rtl_tcp -a 127.0.0.1`; many SDR programs and servers speak the same protocol). `gygax track --live --sdr HOST[:PORT]` retunes it every second to the carrier plus the predicted Doppler shift, captures a quarter of a second, and reports the strongest signal: its offset from the predicted carrier and its height above the noise floor. With good elements the satellite sits at the centre; what remains is the transmitter's own frequency error and your dongle's (`--sdr-ppm` corrects the latter).

```
$ gygax track --tle examples/satlink/sample.tle --at 51.4779,-0.0015,46 --start 2024-01-01T15:00 \
      --live --downlink 437.8e6 --sdr 127.0.0.1:1234 --sdr-rate 250000
sdr     rtl_tcp 127.0.0.1:1234  ·  R820T tuner  ·  250000 S/s  ·  30.5 Hz bins

  time (UTC)    azimuth      elev      range     rate km/s   doppler       receive Hz   sdr peak     snr
  15:00:00    260.2° W     +15.7°   1193.7 km   -6.649     +9.71 kHz      437809710       +350 Hz  92.8 dB
  15:00:01    260.2° W     +15.8°   1187.0 km   -6.644     +9.70 kHz      437809702       +350 Hz  93.3 dB
```

(That trace is from the test suite's simulated sky: a fake `rtl_tcp` that plays the ISS's true Doppler-shifted carrier with a 350 Hz transmitter offset. The Doppler is gone; the 350 Hz is found to within a bin.)

`--sdr-rate` sets the sample rate (default 1.024 MS/s), `--sdr-gain DB` a manual gain (default: the tuner's AGC). From code:

- **C++** (`gygax/satlink/sdr.hpp`): `RtlTcpClient` (greeting and tuner check, the rtl_tcp commands, `read` and `discard` of IQ), `powerSpectrumDb` (averaged Hann-windowed radix-2 FFT, scaled so a full-scale tone reads 0 dB), `strongestPeak` (parabolic interpolation between bins, SNR against the median bin), and `DopplerMixer`, which removes a frequency offset that changes linearly through each block while keeping phase continuous across blocks: feed it the predicted Doppler at the start and end of each block and a carrier sweeping ±10 kHz comes out as a steady tone at zero.
- **Python**: `satlink.RtlTcp` (pure standard library; returns numpy arrays when numpy is installed).

rtl_tcp streams continuously, so samples queued from before a retune arrive first; `discard` them before measuring (the CLI drops 50 ms). Gygax never transmits through an SDR.

## Accuracy

- **Against the reference.** The unit tests propagate all 33 element sets of Vallado's verification set (`tests/fixtures/sgp4/`, near-Earth and deep-space, including the resonance, Lyddane, low-perigee and decay cases) to every time in the published output `tcppver.out`, 667 states in all. The largest position difference is 0.12 mm and the largest velocity difference 0.0009 mm/s; the reference prints 0.01 mm. The error cases fail as the reference does (decay, error 6; out-of-range eccentricity, error 3), as an `Sgp4Error` naming the cause.
- **Against Skyfield.** Over three days of ISS positions seen from Greenwich, sampled every 7.3 minutes, SatLink and Skyfield 1.55 (which adds polar motion and a more rigorous frame chain) agree to 0.0003° in elevation, 0.0002° in azimuth, 2.5 m in range and 4 cm/s in range rate, and find the same 12 rise, culmination and set events.
- **Against the sky.** SGP4 is only as good as the element set: typically about a kilometre at epoch, growing by one to a few kilometres per day for low orbits. That, not the arithmetic above, sets real-world pointing and Doppler accuracy. A kilometre along track puts the ISS 0.1° off at 500 km range and, overhead, where its Doppler at 437.8 MHz sweeps at up to 177 Hz/s, about 25 Hz off.
- **Time.** Unix time does not count leap seconds, and UT1 is taken as UTC. The difference (under 0.9 s) moves the Earth-fixed frame by under 0.4 km at the equator.

## Performance

Measured through the C ABI on one core of an AMD Ryzen 5 2600 (RelWithDebInfo): a complete look (propagation, frame rotation, look angles and range rate) takes about 0.5 µs for the ISS and 0.7 µs for a geostationary satellite; finding the 34 ISS passes over a station in a week takes 17 ms. A tracking loop that updates once a second spends its time waiting.
