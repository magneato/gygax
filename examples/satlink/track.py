#!/usr/bin/env python3
"""Next passes of a satellite over a station, with the Doppler-corrected tuning for each.

    PYTHONPATH=build/python python3 examples/satlink/track.py examples/satlink/sample.tle \\
        --at 51.4779,-0.0015,46 --start 2024-01-01T12:00 --downlink 437.8e6

Add --rig 127.0.0.1:4532 to retune a radio through Hamlib rigctld through the next pass
(rigctld -m <model> -r <device> must be running; -m 1 is Hamlib's dummy rig).
"""

import argparse
import time
from datetime import datetime, timezone

from gygax import satlink


def utc(t: satlink.Time) -> str:
    return t.utc_datetime().strftime("%H:%M:%S")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tle")
    ap.add_argument("--sat", default=None, help="name substring (default: the first set in the file)")
    ap.add_argument("--at", required=True, help="LAT,LON[,METRES]")
    ap.add_argument("--start", default=None, help="UTC, YYYY-MM-DDTHH:MM (default: now)")
    ap.add_argument("--hours", type=float, default=12.0)
    ap.add_argument("--min-elevation", type=float, default=10.0)
    ap.add_argument("--downlink", type=float, default=145.8e6, help="carrier in Hz")
    ap.add_argument("--rig", default=None, help="HOST[:PORT] of rigctld: follow the next pass live")
    args = ap.parse_args()

    with open(args.tle) as f:
        sats = satlink.read_tles(f.read())
    sat = next((s for s in sats if args.sat is None or args.sat.lower() in s.name.lower()), None)
    if sat is None:
        raise SystemExit("no satellite matching %r" % args.sat)
    lat, lon, *rest = (float(x) for x in args.at.split(","))
    station = satlink.Observer(lat, lon, rest[0] if rest else 0.0)
    ts = satlink.load.timescale()
    start = ts.now() if args.start is None else ts.from_datetime(datetime.fromisoformat(args.start).replace(tzinfo=timezone.utc))

    passes = sat.passes(station, start, start.unix + args.hours * 3600, args.min_elevation)
    print("%s: %d passes above %g° in %g h from %s" % (sat.name, len(passes), args.min_elevation, args.hours, start.utc_iso()))
    for p in passes:
        rise = sat.doppler_shift_hz(station, p.rise, args.downlink)
        fall = sat.doppler_shift_hz(station, p.set, args.downlink)
        print("  %s  rise %s at %3.0f°  peak %4.1f° at %s  set %s at %3.0f°  tune %.0f → %.0f Hz"
              % (p.rise.utc_datetime().date(), utc(p.rise), p.rise_azimuth_deg, p.max_elevation_deg, utc(p.culmination),
                 utc(p.set), p.set_azimuth_deg, args.downlink + rise, args.downlink + fall))

    if args.rig and passes:
        host, _, port = args.rig.partition(":")
        p = next((p for p in passes if p.set.unix > time.time()), None)
        if p is None:
            raise SystemExit("no pass still to come in that window (live tracking follows the clock)")
        print("following %s from %s; Ctrl-C to stop" % (sat.name, p.rise.utc_iso()))
        with satlink.Rig(host, int(port or satlink.rig.DEFAULT_PORT)) as rig:
            while time.time() < p.set.unix:
                now = max(time.time(), p.rise.unix)
                time.sleep(max(0.0, now - time.time()))
                look = sat.observe(station, now)
                rig.set_frequency(args.downlink + look.doppler_shift_hz(args.downlink))
                print("  %s  az %5.1f°  el %4.1f°  %+8.1f Hz" % (utc(satlink.Time(now)), look.azimuth_deg, look.elevation_deg,
                                                               look.doppler_shift_hz(args.downlink)))
                time.sleep(1.0 - time.time() % 1.0)


if __name__ == "__main__":
    main()
