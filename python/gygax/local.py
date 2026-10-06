import os
import shutil
import signal
import socket
import subprocess
import time
from typing import List, Optional

from .client import Client

LOOPBACK_HOST = "127.0.0.1"
STARTUP_POLL_INTERVAL_SECONDS = 0.05
SHUTDOWN_TIMEOUT_SECONDS = 10


def find_binary() -> str:
    candidate = os.environ.get("GYGAX_BIN") or shutil.which("gygax")
    if not candidate or not os.path.exists(candidate):
        raise FileNotFoundError("gygax binary not found; set GYGAX_BIN or put gygax on PATH")
    return candidate


def _free_port() -> int:
    with socket.socket() as s:
        s.bind((LOOPBACK_HOST, 0))
        return s.getsockname()[1]


class LocalService:
    def __init__(
        self,
        token: str = "",
        engines: Optional[List[str]] = None,
        plugins: Optional[List[str]] = None,
        extensions: Optional[List[str]] = None,
        ledger: Optional[str] = None,
        device_commands: bool = False,
        extra_args: Optional[List[str]] = None,
        startup_timeout: float = 20.0,
    ):
        self.token = token
        self.port = _free_port()
        self.url = f"http://{LOOPBACK_HOST}:{self.port}"
        args = [find_binary(), "serve", "--host", LOOPBACK_HOST, "--port", str(self.port), "--log-level", "warn"]
        for engine in engines or ["echo"]:
            args += ["--engine", engine]
        for plugin in plugins or []:
            args += ["--plugin", plugin]
        for extension in extensions or []:
            args += ["--extension", extension]
        if ledger:
            args += ["--ledger", ledger]
        if device_commands:
            args.append("--device-commands")
        args += extra_args or []
        env = dict(os.environ)
        env.pop("GYGAX_API_TOKEN", None)
        if token:
            env["GYGAX_API_TOKEN"] = token
        self._process = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.client = Client(self.url, token)
        deadline = time.time() + startup_timeout
        while time.time() < deadline:
            if self._process.poll() is not None:
                raise RuntimeError("gygax exited during startup: " + self._process.stderr.read().decode("utf-8", "replace"))
            if self.client.health():
                return
            time.sleep(STARTUP_POLL_INTERVAL_SECONDS)
        self.stop()
        raise TimeoutError("gygax did not become healthy in time")

    def stop(self) -> None:
        if self._process.poll() is None:
            self._process.send_signal(signal.SIGTERM)
            try:
                self._process.wait(timeout=SHUTDOWN_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait()
        if self._process.stderr:
            self._process.stderr.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()
