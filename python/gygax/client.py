import json
import os
import urllib.error
import urllib.request
from typing import Any, Dict, Iterator, List, Optional

DEFAULT_REQUEST_TIMEOUT_SECONDS = 180.0
DEFAULT_AGENT_MAX_STEPS = 8
DEFAULT_AGENT_WAIT_SECONDS = 120.0
DEFAULT_SERVICE_URL = "http://127.0.0.1:1984"
DEFAULT_LOGISTICS_PAGE_LIMIT = 200
HTTP_INTERNAL_SERVER_ERROR = 500
JSON_RPC_VERSION = "2.0"


class ServiceError(RuntimeError):
    def __init__(self, status: int, code: str, message: str):
        super().__init__(f"HTTP {status} {code}: {message}")
        self.status = status
        self.code = code
        self.message = message


class Client:
    def __init__(self, url: Optional[str] = None, token: Optional[str] = None, timeout: float = DEFAULT_REQUEST_TIMEOUT_SECONDS):
        self.url = (url or os.environ.get("GYGAX_URL") or DEFAULT_SERVICE_URL).rstrip("/")
        self.token = token if token is not None else os.environ.get("GYGAX_API_TOKEN", "")
        self.timeout = timeout

    def _request(self, method: str, path: str, body: Optional[dict] = None, stream: bool = False):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.url + path, data=data, method=method)
        req.add_header("Content-Type", "application/json")
        req.add_header("Accept", "application/json")
        if self.token:
            req.add_header("Authorization", f"Bearer {self.token}")
        try:
            resp = urllib.request.urlopen(req, timeout=self.timeout)
        except urllib.error.HTTPError as e:
            payload = e.read().decode("utf-8", "replace")
            try:
                err = json.loads(payload).get("error", {})
            except ValueError:
                err = {}
            raise ServiceError(e.code, err.get("code", "http_error"), err.get("message", payload)) from None
        if stream:
            return resp
        with resp:
            raw = resp.read().decode("utf-8")
        return json.loads(raw) if raw else {}

    def health(self) -> bool:
        try:
            return self._request("GET", "/healthz").get("status") == "ok"
        except (ServiceError, OSError):
            return False

    def node(self) -> Dict[str, Any]:
        return self._request("GET", "/v1/node")

    def engines(self) -> List[Dict[str, Any]]:
        return self._request("GET", "/v1/engines")["engines"]

    def models(self) -> List[str]:
        return [m["id"] for m in self._request("GET", "/v1/models")["data"]]

    def tools(self) -> List[Dict[str, str]]:
        return self._request("GET", "/v1/tools")["tools"]

    def invoke_tool(self, name: str, tool_input: Any = "") -> str:
        return self._request("POST", f"/v1/tools/{name}/invoke", {"input": tool_input})["output"]

    def chat(self, prompt: str, model: str = "", system: Optional[str] = None, **options) -> str:
        messages = []
        if system:
            messages.append({"role": "system", "content": system})
        messages.append({"role": "user", "content": prompt})
        body = {"model": model, "messages": messages, "stream": False, **options}
        result = self._request("POST", "/v1/chat/completions", body)
        return result["choices"][0]["message"]["content"]

    def chat_stream(self, prompt: str, model: str = "", **options) -> Iterator[str]:
        body = {"model": model, "messages": [{"role": "user", "content": prompt}], "stream": True, **options}
        resp = self._request("POST", "/v1/chat/completions", body, stream=True)
        with resp:
            for raw in resp:
                line = raw.decode("utf-8").strip()
                if not line.startswith("data:"):
                    continue
                payload = line[5:].strip()
                if payload == "[DONE]":
                    return
                delta = json.loads(payload)["choices"][0]["delta"].get("content")
                if delta:
                    yield delta

    def agent(self, objective: str, model: str = "", max_steps: int = DEFAULT_AGENT_MAX_STEPS,
              wait_seconds: float = DEFAULT_AGENT_WAIT_SECONDS) -> Dict[str, Any]:
        created = self._request("POST", "/v1/agents", {"max_steps": max_steps})
        sid = created["id"]
        try:
            return self._request(
                "POST", f"/v1/agents/{sid}/objectives", {"objective": objective, "model": model, "wait_seconds": wait_seconds}
            )
        finally:
            try:
                self._request("DELETE", f"/v1/agents/{sid}")
            except ServiceError:
                pass

    def simulate(self, spec: dict) -> Dict[str, Any]:
        return self._request("POST", "/v1/neuro/simulate", spec)

    def rpc(self, method: str, params: Optional[dict] = None, request_id: int = 1) -> Any:
        reply = self._request(
            "POST", "/rpc", {"jsonrpc": JSON_RPC_VERSION, "id": request_id, "method": method, "params": params or {}}
        )
        if "error" in reply:
            raise ServiceError(
                reply["error"].get("data", {}).get("status", HTTP_INTERNAL_SERVER_ERROR), "rpc_error", reply["error"]["message"]
            )
        return reply["result"]

    def devices(self) -> Dict[str, Any]:
        return self._request("GET", "/v1/devices")

    def add_device(self, device_id: str, kind: str, **config) -> Dict[str, Any]:
        return self._request("POST", "/v1/devices", {"id": device_id, "kind": kind, **config})

    def remove_device(self, device_id: str) -> Dict[str, Any]:
        return self._request("DELETE", f"/v1/devices/{device_id}")

    def device_state(self, device_id: str) -> Dict[str, Any]:
        return self._request("GET", f"/v1/devices/{device_id}/state")

    def device_command(self, device_id: str, command: str, **fields) -> Dict[str, Any]:
        return self._request("POST", f"/v1/devices/{device_id}/command", {"command": command, **fields})

    @property
    def logistics(self) -> "Logistics":
        return Logistics(self)


class Logistics:
    def __init__(self, client: Client):
        self._c = client

    def put_model(self, sku: str, max_range_m: float = 0.0, cruise_mps: float = 0.0, kind: str = "", power: str = "", **attrs) -> Dict[str, Any]:
        body: Dict[str, Any] = {"sku": sku, "max_range_m": max_range_m, "cruise_mps": cruise_mps, "kind": kind, "power": power}
        if attrs:
            body["attrs"] = attrs
        return self._c._request("POST", "/v1/logistics/models", body)

    def models(self) -> List[Dict[str, Any]]:
        return self._c._request("GET", "/v1/logistics/models")["models"]

    def put_site(self, site_id: str, latitude: float, longitude: float, kind: str = "refuel", name: str = "", services: Optional[List[str]] = None) -> Dict[str, Any]:
        body: Dict[str, Any] = {"id": site_id, "latitude": latitude, "longitude": longitude, "kind": kind, "services": services or []}
        if name:
            body["name"] = name
        return self._c._request("POST", "/v1/logistics/sites", body)

    def sites(self) -> List[Dict[str, Any]]:
        return self._c._request("GET", "/v1/logistics/sites")["sites"]

    def remove_site(self, site_id: str) -> Dict[str, Any]:
        return self._c._request("DELETE", f"/v1/logistics/sites/{site_id}")

    def record(self, line: Optional[str] = None, **fields) -> Dict[str, Any]:
        body: Dict[str, Any] = dict(fields)
        if line is not None:
            body["line"] = line
        return self._c._request("POST", "/v1/logistics/events", body)

    def events(self, sku: str = "", since: str = "", limit: int = DEFAULT_LOGISTICS_PAGE_LIMIT) -> List[Dict[str, Any]]:
        return self._c._request("GET", "/v1/logistics/events" + _query(sku=sku, since=since, limit=limit))["events"]

    def units(self, sku: str = "", status: str = "", limit: int = DEFAULT_LOGISTICS_PAGE_LIMIT) -> List[Dict[str, Any]]:
        return self._c._request("GET", "/v1/logistics/units" + _query(sku=sku, status=status, limit=limit))["units"]

    def unit(self, guid: str) -> Dict[str, Any]:
        return self._c._request("GET", f"/v1/logistics/units/{guid}")

    def update_unit(self, guid: str, **patch) -> Dict[str, Any]:
        return self._c._request("POST", f"/v1/logistics/units/{guid}", patch)

    def summary(self, since: str = "7d") -> Dict[str, Any]:
        return self._c._request("GET", "/v1/logistics/summary" + _query(since=since))

    def plan(self, waypoints: List[Dict[str, float]], **options) -> Dict[str, Any]:
        return self._c._request("POST", "/v1/logistics/plan", {"waypoints": waypoints, **options})


def _query(**params) -> str:
    from urllib.parse import urlencode

    pairs = {k: v for k, v in params.items() if v not in ("", None)}
    return "?" + urlencode(pairs) if pairs else ""
