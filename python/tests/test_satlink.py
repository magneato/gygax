import math
import os
import socket
import struct
import subprocess
import threading
import time
import unittest
from datetime import datetime, timezone

from gygax import satlink
from gygax.satlink import Observer, Rig, RigError, SatLinkError, Satellite

ISS = (
    "1 25544U 98067A   24001.50000000  .00016717  00000-0  30306-3 0  9999",
    "2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.49815335432866",
)
VANGUARD = (
    "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
    "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667",
)
GREENWICH = Observer(51.4779, -0.0015, 46.0)


class SatLinkPredictTests(unittest.TestCase):
    def setUp(self):
        self.iss = Satellite(*ISS, name="ISS (ZARYA)")

    def tearDown(self):
        self.iss.close()

    def test_elements_and_epoch(self):
        self.assertEqual(self.iss.name, "ISS (ZARYA)")
        self.assertEqual(self.iss.epoch.utc_datetime(), datetime(2024, 1, 1, 12, tzinfo=timezone.utc))
        self.assertAlmostEqual(self.iss.period_minutes, 92.914, places=3)
        self.assertFalse(self.iss.deep_space)

    def test_matches_the_sgp4_reference(self):
        # Vanguard 1 at epoch and 360 minutes later, from Vallado's tcppver.out (TEME, km and km/s).
        with Satellite(*VANGUARD) as v:
            r, vel = v.teme(v.epoch)
            for got, want in zip(r + vel, (7022.46529266, -1400.08296755, 0.03995155, 1.893841015, 6.405893759, 4.534807250)):
                self.assertAlmostEqual(got, want, delta=1e-6)
            r, _ = v.teme(v.epoch.unix + 360 * 60)
            for got, want in zip(r, (-7154.03120202, -3783.17682504, -3536.19412294)):
                self.assertAlmostEqual(got, want, delta=1e-6)

    def test_bad_elements_raise_with_the_reason(self):
        line2 = ISS[1][:20] + "9" + ISS[1][21:]
        with self.assertRaisesRegex(SatLinkError, "checksum"):
            Satellite(ISS[0], line2)

    def test_look_angles_doppler_and_skyfield_shape(self):
        ts = satlink.load.timescale()
        t = ts.utc(2024, 1, 1, 15, 0, 0)
        look = self.iss.observe(GREENWICH, t)
        self.assertTrue(-90 <= look.elevation_deg <= 90)
        self.assertTrue(0 <= look.azimuth_deg < 360)
        self.assertGreater(look.range_km, 400)

        alt, az, distance = (self.iss - GREENWICH).at(t).altaz()
        self.assertAlmostEqual(alt.degrees, look.elevation_deg)
        self.assertAlmostEqual(az.degrees, look.azimuth_deg)
        self.assertAlmostEqual(distance.km, look.range_km)

        sky = satlink.EarthSatellite(ISS[0], ISS[1], "ISS", ts)
        here = satlink.wgs84.latlon(51.4779, -0.0015, elevation_m=46.0)
        self.assertAlmostEqual(sky.observe(here, datetime(2024, 1, 1, 15, tzinfo=timezone.utc)).range_km, look.range_km)

        shift = satlink.doppler_shift_hz(self.iss, GREENWICH, t, 437.8e6)
        self.assertAlmostEqual(shift, -437.8e6 * look.range_rate_km_s / satlink.SPEED_OF_LIGHT_KM_S, places=6)
        self.assertLess(abs(shift), 11_500)

    def test_passes_and_find_events(self):
        start = self.iss.epoch
        passes = self.iss.passes(GREENWICH, start, start.unix + 86400, 10.0)
        self.assertGreater(len(passes), 1)
        for p in passes:
            self.assertLess(p.rise, p.culmination)
            self.assertLess(p.culmination, p.set)
            self.assertGreaterEqual(p.max_elevation_deg, 10.0)
            self.assertAlmostEqual(self.iss.observe(GREENWICH, p.rise).elevation_deg, 10.0, places=2)
            self.assertGreater(self.iss.doppler_shift_hz(GREENWICH, p.rise, 145.8e6), 0)
            self.assertLess(self.iss.doppler_shift_hz(GREENWICH, p.set, 145.8e6), 0)

        times, events = self.iss.find_events(GREENWICH, start, start.unix + 86400, altitude_degrees=10.0)
        self.assertEqual(len(times), len(events))
        self.assertEqual(events[:3], [0, 1, 2])
        self.assertEqual(times[0], passes[0].rise)

    def test_subpoint_and_ecef(self):
        sub = self.iss.subpoint(self.iss.epoch)
        self.assertTrue(400 < sub.altitude_km < 445)
        self.assertLessEqual(abs(sub.latitude_deg), 51.9)
        r, v = self.iss.ecef(self.iss.epoch)
        self.assertAlmostEqual(math.dist((0, 0, 0), r), 6378 + sub.altitude_km, delta=25)

    def test_read_tles_handles_three_line_files(self):
        text = "ISS (ZARYA)\n%s\n%s\n\n0 VANGUARD 1\n%s\n%s\n" % (ISS + VANGUARD)
        sats = satlink.read_tles(text)
        self.assertEqual([s.name for s in sats], ["ISS (ZARYA)", "VANGUARD 1"])

    def test_compatibility_alias(self):
        import satlink as alias
        import satlink.predict
        import satlink.rig

        self.assertIs(alias.Satellite, Satellite)
        self.assertIs(satlink.rig.Rig, Rig)


class FakeRigctld:
    """Loopback rigctld answering lines from a script."""

    def __init__(self, replies):
        self.replies = list(replies)
        self.received = []
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        conn, _ = self.server.accept()
        with conn:
            buffer = b""
            while self.replies:
                chunk = conn.recv(256)
                if not chunk:
                    break
                buffer += chunk
                while b"\n" in buffer and self.replies:
                    line, _, buffer = buffer.partition(b"\n")
                    self.received.append(line.decode())
                    conn.sendall(self.replies.pop(0).encode() + b"\n")
        self.server.close()

    def join(self):
        self.thread.join(5)
        return self.received


class SatLinkRigTests(unittest.TestCase):
    def test_rig_keeps_one_connection(self):
        fake = FakeRigctld(["RPRT 0", "RPRT 0", "145801234"])
        with Rig("127.0.0.1", fake.port) as rig:
            rig.set_frequency(145_800_000.4)
            rig.set_frequency(145_801_234)
            self.assertEqual(rig.frequency(), 145_801_234.0)
        self.assertEqual(fake.join(), ["F 145800000", "F 145801234", "f"])

    def test_refusal_raises(self):
        fake = FakeRigctld(["RPRT -11"])
        with Rig("127.0.0.1", fake.port) as rig, self.assertRaisesRegex(RigError, "RPRT -11"):
            rig.set_frequency(437_800_000)
        fake.join()

    def test_one_shot_python_and_native(self):
        fake = FakeRigctld(["RPRT 0"])
        self.assertEqual(satlink.set_rig_frequency("127.0.0.1", fake.port, 437_800_000), "RPRT 0")
        self.assertEqual(fake.join(), ["F 437800000"])

        from gygax._native import lib
        import ctypes

        fake = FakeRigctld(["RPRT 0"])
        raw = lib().gygax_satlink_rig_set_freq(b"127.0.0.1", fake.port, 145_800_000.0, 2000)
        self.assertTrue(raw)
        self.assertEqual(ctypes.string_at(raw), b"RPRT 0")
        lib().gygax_free(raw)
        self.assertEqual(fake.join(), ["F 145800000"])


SAMPLE_TLE = os.path.join(os.path.dirname(__file__), "..", "..", "examples", "satlink", "sample.tle")


class FakeSky:
    """An rtl_tcp server whose antenna sees one satellite.

    It streams, in real time, a carrier at the frequency a station would truly receive: the
    transmitter's frequency (``carrier_hz + tx_offset_hz``) shifted by the satellite's Doppler at
    the moment of each retune. The n-th retune is taken to happen at ``start + n - 1`` seconds,
    the cadence of ``gygax track --live`` with a fixed start.
    """

    TABLE = 4096

    def __init__(self, sat, observer, start_unix, carrier_hz, tx_offset_hz):
        self.sat, self.observer, self.start = sat, observer, start_unix
        self.carrier, self.tx = carrier_hz, tx_offset_hz
        self.rate, self.center, self.tunes = 250_000, None, 0
        self.offset = 0.0
        self.cos = [round(127.5 + 90 * math.cos(2 * math.pi * k / self.TABLE)) for k in range(self.TABLE)]
        self.sin = [round(127.5 + 90 * math.sin(2 * math.pi * k / self.TABLE)) for k in range(self.TABLE)]
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _command(self, op, arg):
        if op == 0x02:
            self.rate = arg
        elif op == 0x01:
            self.center, self.tunes = arg, self.tunes + 1
            when = self.start + self.tunes - 1
            received = self.carrier + self.tx + self.sat.doppler_shift_hz(self.observer, when, self.carrier)
            self.offset = received - self.center

    def _serve(self):
        conn, _ = self.server.accept()
        conn.sendall(b"RTL0" + struct.pack(">II", 5, 29))
        conn.setblocking(False)
        pending, phase, block = b"", 0.0, 2500
        began, sent = time.monotonic(), 0
        try:
            while not self.stop.is_set():
                try:
                    data = conn.recv(4096)
                    if not data:
                        break
                    pending += data
                except BlockingIOError:
                    pass
                while len(pending) >= 5:
                    self._command(*struct.unpack(">BI", pending[:5]))
                    pending = pending[5:]
                step = self.offset / self.rate * self.TABLE
                out = bytearray(2 * block)
                for i in range(block):
                    k = int(phase) % self.TABLE
                    out[2 * i], out[2 * i + 1] = self.cos[k], self.sin[k]
                    phase += step
                phase %= self.TABLE
                conn.setblocking(True)
                conn.sendall(out)
                conn.setblocking(False)
                sent += block
                ahead = sent / self.rate - (time.monotonic() - began)  # pace at the sample rate
                if ahead > 0:
                    time.sleep(ahead)
        except OSError:
            pass
        finally:
            conn.close()
            self.server.close()

    def close(self):
        self.stop.set()
        self.thread.join(5)


class SatLinkSdrTests(unittest.TestCase):
    def setUp(self):
        self.iss = Satellite(*ISS, name="ISS")
        self.start = satlink.timescale.utc(2024, 1, 1, 15, 0, 0).unix

    def tearDown(self):
        self.iss.close()

    def test_python_client_hears_the_carrier(self):
        sky = FakeSky(self.iss, GREENWICH, self.start, 437.8e6, tx_offset_hz=0.0)
        try:
            with satlink.RtlTcp("127.0.0.1", sky.port) as sdr:
                self.assertEqual((sdr.tuner, sdr.gain_stages), ("R820T", 29))
                sdr.set_sample_rate(250_000)
                shift = self.iss.doppler_shift_hz(GREENWICH, self.start, 437.8e6)
                sdr.set_center_frequency(437.8e6 + shift + 20_000)  # tuned 20 kHz high on purpose
                sdr.discard(25_000)
                iq = list(sdr.read(4096))
        finally:
            sky.close()
        # A 4096-point DFT at the expected bin, against its neighbours.
        def power(hz):
            w = -2j * math.pi * hz / 250_000
            return abs(sum(x * complex(math.cos((w * k).imag), math.sin((w * k).imag)) for k, x in enumerate(iq)))
        self.assertGreater(power(-20_000), 10 * power(-15_000))

    @unittest.skipUnless(os.environ.get("GYGAX_BIN"), "GYGAX_BIN not set")
    def test_gygax_track_follows_the_carrier_with_an_sdr(self):
        tx = 350.0  # the transmitter is a little off frequency, as real ones are
        sky = FakeSky(self.iss, GREENWICH, self.start, 437.8e6, tx_offset_hz=tx)
        try:
            out = subprocess.run(
                [os.environ["GYGAX_BIN"], "track", "--tle", SAMPLE_TLE, "--at", "51.4779,-0.0015,46",
                 "--start", "2024-01-01T15:00", "--live", "--count", "3", "--downlink", "437.8e6",
                 "--sdr", "127.0.0.1:%d" % sky.port, "--sdr-rate", "250000"],
                capture_output=True, text=True, timeout=60)
        finally:
            sky.close()
        self.assertEqual(out.returncode, 0, out.stderr)
        rows = [line for line in out.stdout.splitlines() if line.rstrip().endswith("dB")]
        self.assertEqual(len(rows), 3, out.stdout)
        bin_hz = 250_000 / 8192
        for row in rows:
            fields = row.split()
            peak, snr = float(fields[-4]), float(fields[-2])
            self.assertAlmostEqual(peak, tx, delta=bin_hz, msg=row)  # Doppler gone, only the transmitter's offset left
            self.assertGreater(snr, 30.0, row)


if __name__ == "__main__":
    unittest.main()
