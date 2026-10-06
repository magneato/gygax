import hmac
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Callable, Dict, List, Optional, Tuple

MAX_REQUEST_BODY_BYTES = 8 * 1024 * 1024
MAX_EXTENSION_NAME_LENGTH = 32
MAX_TOOL_NAME_LENGTH = 48
MAX_REGISTERED_TOOLS = 128
DEFAULT_BIND_HOST = "127.0.0.1"
DEFAULT_STOP_TIMEOUT_SECONDS = 5
HTTP_OK = 200
HTTP_BAD_REQUEST = 400
HTTP_UNAUTHORIZED = 401
HTTP_NOT_FOUND = 404
HTTP_PAYLOAD_TOO_LARGE = 413
HTTP_INTERNAL_SERVER_ERROR = 500
LOOPBACK = (DEFAULT_BIND_HOST, "::1", "localhost")


class ExtensionError(RuntimeError):
    def __init__(self, message: str, status: int = HTTP_INTERNAL_SERVER_ERROR):
        super().__init__(message)
        self.status = status


class _Tool:
    def __init__(self, name: str, description: str, fn: Callable[..., Any], json_input: bool):
        self.name = name
        self.description = description
        self.fn = fn
        self.json_input = json_input


class RunningExtension:
    def __init__(self, server: ThreadingHTTPServer, thread: threading.Thread, host: str):
        self._server = server
        self._thread = thread
        self.host = host
        self.port = server.server_address[1]

    @property
    def url(self) -> str:
        host = f"[{self.host}]" if ":" in self.host else self.host
        return f"http://{host}:{self.port}"

    def stop(self) -> None:
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=DEFAULT_STOP_TIMEOUT_SECONDS)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


class Extension:
    def __init__(self, name: str, version: str = "0.0.0", token: Optional[str] = None):
        if not name or len(name) > MAX_EXTENSION_NAME_LENGTH or not all(c.isascii() and (c.islower() or c.isdigit() or c in "_-") for c in name):
            raise ValueError("name must be 1-32 characters of [a-z0-9_-]")
        self.name = name
        self.version = version
        self.token = token or ""
        self._tools: Dict[str, _Tool] = {}

    def tool(self, name: Optional[str] = None, description: Optional[str] = None, json_input: bool = False):
        def decorate(fn: Callable[..., Any]):
            tool_name = name or fn.__name__
            if not tool_name or len(tool_name) > MAX_TOOL_NAME_LENGTH or not all(c.isascii() and (c.isalnum() or c in "_-") for c in tool_name):
                raise ValueError("tool name must be 1-48 characters of [A-Za-z0-9_-]")
            if tool_name in self._tools:
                raise ValueError(f"tool '{tool_name}' is already defined")
            if len(self._tools) >= MAX_REGISTERED_TOOLS:
                raise ValueError("at most 128 tools")
            doc = (description or fn.__doc__ or "").strip().splitlines()
            self._tools[tool_name] = _Tool(tool_name, description or (doc[0] if doc else ""), fn, json_input)
            return fn

        return decorate

    def describe(self) -> Dict[str, Any]:
        return {
            "name": self.name,
            "version": self.version,
            "tools": [{"name": t.name, "description": t.description} for t in self._tools.values()],
        }

    def call(self, tool: str, tool_input: str) -> str:
        entry = self._tools.get(tool)
        if entry is None:
            raise ExtensionError(f"unknown tool '{tool}'", HTTP_NOT_FOUND)
        try:
            argument: Any = json.loads(tool_input) if entry.json_input else tool_input
        except ValueError as exc:
            raise ExtensionError(f"input must be JSON: {exc}", HTTP_BAD_REQUEST) from None
        try:
            result = entry.fn(argument)
        except ExtensionError:
            raise
        except Exception as exc:
            raise ExtensionError(f"{type(exc).__name__}: {exc}", HTTP_INTERNAL_SERVER_ERROR) from None
        return result if isinstance(result, str) else json.dumps(result)

    def _authorized(self, headers: Dict[str, str]) -> bool:
        if not self.token:
            return True
        supplied = headers.get("authorization", "")
        return hmac.compare_digest(supplied.encode(), f"Bearer {self.token}".encode())

    def handle(self, method: str, path: str, headers: Dict[str, str], body: bytes) -> Tuple[int, Dict[str, Any]]:
        lowered = {k.lower(): v for k, v in headers.items()}
        if not self._authorized(lowered):
            return HTTP_UNAUTHORIZED, {"error": "unauthorized"}
        if method == "GET" and path == "/gygax/describe":
            return HTTP_OK, self.describe()
        prefix = "/gygax/call/"
        if method == "POST" and path.startswith(prefix):
            if len(body) > MAX_REQUEST_BODY_BYTES:
                return HTTP_PAYLOAD_TOO_LARGE, {"error": "payload too large"}
            try:
                payload = json.loads(body.decode("utf-8") or "{}")
            except ValueError:
                return HTTP_BAD_REQUEST, {"error": "body must be JSON"}
            if not isinstance(payload, dict) or not isinstance(payload.get("input", ""), str):
                return HTTP_BAD_REQUEST, {"error": "body must be {\"input\": string}"}
            try:
                return HTTP_OK, {"output": self.call(path[len(prefix):], payload.get("input", ""))}
            except ExtensionError as exc:
                return exc.status, {"error": str(exc)}
        return HTTP_NOT_FOUND, {"error": "not found"}

    def serve(self, host: str = DEFAULT_BIND_HOST, port: int = 0, block: bool = False) -> Optional[RunningExtension]:
        if host not in LOOPBACK and not self.token:
            raise ValueError("a token is required to listen on a non-loopback address")
        extension = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def _respond(self, status: int, payload: Dict[str, Any]) -> None:
                data = json.dumps(payload).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def _dispatch(self, method: str) -> None:
                length = int(self.headers.get("Content-Length") or 0)
                if length > MAX_REQUEST_BODY_BYTES:
                    self._respond(HTTP_PAYLOAD_TOO_LARGE, {"error": "payload too large"})
                    self.close_connection = True
                    return
                body = self.rfile.read(length) if length else b""
                status, payload = extension.handle(method, self.path.split("?", 1)[0], dict(self.headers.items()), body)
                self._respond(status, payload)

            def do_GET(self):
                self._dispatch("GET")

            def do_POST(self):
                self._dispatch("POST")

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer((host, port), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, name=f"gygax-extension-{self.name}", daemon=True)
        thread.start()
        running = RunningExtension(server, thread, host)
        if block:
            try:
                thread.join()
            except KeyboardInterrupt:
                running.stop()
            return None
        return running
