"""SatLink SDR: an rtl_tcp client (RTL-SDR dongles and the servers that speak its protocol).

    with satlink.RtlTcp("127.0.0.1", 1234) as sdr:
        sdr.set_sample_rate(1_024_000)
        sdr.set_center_frequency(437.8e6 + iss.doppler_shift_hz(home, now, 437.8e6))
        iq = sdr.read(65536)            # complex samples in [-1, 1]; a numpy array if numpy is installed

Pure Python, standard library only; numpy is used for the samples when it is available.
"""

import socket
import struct
from typing import List, Optional

DEFAULT_PORT = 1234
TUNERS = {1: "E4000", 2: "FC0012", 3: "FC0013", 4: "FC2580", 5: "R820T", 6: "R828D"}

try:
    import numpy as _np
except ImportError:
    _np = None


class SdrError(RuntimeError):
    """The rtl_tcp server could not be reached, is not rtl_tcp, or stopped sending."""


class RtlTcp:
    CENTER_FREQUENCY, SAMPLE_RATE, GAIN_MODE, GAIN, FREQUENCY_CORRECTION = 0x01, 0x02, 0x03, 0x04, 0x05
    AGC_MODE, BIAS_TEE = 0x08, 0x0E

    def __init__(self, host: str = "127.0.0.1", port: int = DEFAULT_PORT, timeout: float = 2.0):
        self.host, self.port = host or "127.0.0.1", int(port)
        try:
            self._sock = socket.create_connection((self.host, self.port), timeout=timeout)
        except OSError as e:
            raise SdrError("rtl_tcp at %s:%d: %s" % (self.host, self.port, e)) from None
        greeting = self._recv_exactly(12)
        if greeting[:4] != b"RTL0":
            self.close()
            raise SdrError("%s:%d is not an rtl_tcp server (no RTL0 greeting)" % (self.host, self.port))
        self.tuner_type, self.gain_stages = struct.unpack(">II", greeting[4:])
        self.center_frequency: Optional[float] = None
        self.sample_rate: Optional[int] = None

    @property
    def tuner(self) -> str:
        return TUNERS.get(self.tuner_type, "unknown")

    def _recv_exactly(self, size: int) -> bytes:
        chunks, have = [], 0
        while have < size:
            try:
                chunk = self._sock.recv(min(size - have, 1 << 20))
            except OSError as e:
                raise SdrError("rtl_tcp at %s:%d: %s" % (self.host, self.port, e)) from None
            if not chunk:
                raise SdrError("rtl_tcp at %s:%d closed the connection" % (self.host, self.port))
            chunks.append(chunk)
            have += len(chunk)
        return b"".join(chunks)

    def send(self, command: int, argument: int) -> None:
        try:
            self._sock.sendall(struct.pack(">BI", command, argument & 0xFFFFFFFF))
        except OSError as e:
            raise SdrError("rtl_tcp at %s:%d: %s" % (self.host, self.port, e)) from None

    def set_center_frequency(self, hz: float) -> None:
        if not 0 < hz <= 4e9:
            raise SdrError("cannot tune rtl_tcp to %r Hz" % hz)
        self.send(self.CENTER_FREQUENCY, round(hz))
        self.center_frequency = float(round(hz))

    def set_sample_rate(self, samples_per_second: int) -> None:
        if not 0 < samples_per_second <= 3_200_000:
            raise SdrError("sample rate %r is outside 1..3200000" % samples_per_second)
        self.send(self.SAMPLE_RATE, int(samples_per_second))
        self.sample_rate = int(samples_per_second)

    def set_automatic_gain(self) -> None:
        self.send(self.GAIN_MODE, 0)

    def set_gain_db(self, db: float) -> None:
        self.send(self.GAIN_MODE, 1)
        self.send(self.GAIN, round(db * 10))

    def set_frequency_correction(self, ppm: int) -> None:
        self.send(self.FREQUENCY_CORRECTION, int(ppm))

    def set_bias_tee(self, on: bool) -> None:
        self.send(self.BIAS_TEE, 1 if on else 0)

    def read(self, count: int):
        """The next ``count`` IQ samples scaled to [-1, 1] (numpy complex64 array, or a list)."""
        raw = self._recv_exactly(2 * int(count))
        if _np is not None:
            u = _np.frombuffer(raw, dtype=_np.uint8).astype(_np.float32)
            u = (u - 127.5) / 127.5
            return u[0::2] + 1j * u[1::2]
        return [complex((raw[i] - 127.5) / 127.5, (raw[i + 1] - 127.5) / 127.5) for i in range(0, len(raw), 2)]

    def discard(self, count: int) -> None:
        """Drop samples queued from before a retune."""
        left = 2 * int(count)
        while left:
            left -= len(self._recv_exactly(min(left, 1 << 20)))

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def __enter__(self) -> "RtlTcp":
        return self

    def __exit__(self, *exc) -> None:
        self.close()
