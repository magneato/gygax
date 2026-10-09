"""SatLink radio control: Hamlib rigctld over TCP.

``Rig`` keeps one connection open, so a tracking loop can retune every second without a TCP
handshake each time. ``set_rig_frequency`` is the one-shot form.
"""

import socket
from typing import Optional

DEFAULT_PORT = 4532


class RigError(RuntimeError):
    """rigctld could not be reached, did not answer, or refused a command."""


class Rig:
    def __init__(self, host: str = "127.0.0.1", port: int = DEFAULT_PORT, timeout: float = 2.0):
        self.host = host or "127.0.0.1"
        self.port = int(port)
        self.timeout = float(timeout)
        self._sock: Optional[socket.socket] = None
        self._buffer = b""

    def _connect(self) -> socket.socket:
        if self._sock is None:
            try:
                self._sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
            except OSError as e:
                raise RigError("rigctld at %s:%d: %s" % (self.host, self.port, e)) from None
            self._buffer = b""
        return self._sock

    def _exchange(self, line: str) -> str:
        sock = self._connect()
        try:
            sock.sendall(line.encode("ascii") + b"\n")
            while b"\n" not in self._buffer:
                chunk = sock.recv(4096)
                if not chunk:
                    if self._buffer:
                        break
                    raise RigError("rigctld at %s:%d closed the connection" % (self.host, self.port))
                self._buffer += chunk
        except (OSError, RigError):
            self.close()
            raise
        reply, newline, self._buffer = self._buffer.partition(b"\n")
        if not newline:  # the reply ended with the connection
            self.close()
        return reply.decode("ascii", "replace").rstrip("\r")

    def command(self, line: str) -> str:
        """Send one command line and return the first reply line."""
        fresh = self._sock is None
        try:
            return self._exchange(line)
        except (OSError, RigError):
            if fresh:
                raise
            return self._exchange(line)  # rigctld may have dropped an idle connection

    def set_frequency(self, frequency_hz: float) -> None:
        reply = self.command("F %d" % round(frequency_hz))
        if reply != "RPRT 0":
            raise RigError("rigctld refused F %d: %s" % (round(frequency_hz), reply))

    def frequency(self) -> float:
        reply = self.command("f")
        try:
            return float(reply)
        except ValueError:
            raise RigError("rigctld answered f with %r" % reply) from None

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def __enter__(self) -> "Rig":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def set_rig_frequency(host: str, port: int, frequency_hz: float, timeout: float = 2.0) -> str:
    """Send ``F <hz>`` once and return rigctld's reply (``"RPRT 0"`` on success)."""
    with Rig(host, port, timeout) as rig:
        return rig.command("F %d" % round(frequency_hz))
