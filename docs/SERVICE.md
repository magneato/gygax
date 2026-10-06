# Service reference

`gygax serve` exposes one HTTP/1.1 listener (default `127.0.0.1:1984`). Responses use `Connection: close`; request bodies may use `Content-Length` or chunked encoding and `Expect: 100-continue` is honoured. Every response carries `X-Request-Id` (echoed if you send a valid one).

## Authentication

Set `GYGAX_API_TOKEN` (or `--token-file`). Everything except `/healthz`, `/readyz` and `/version` then requires `Authorization: Bearer <token>`. The comparison is constant time. Without a token the service only starts on loopback addresses unless `GYGAX_ALLOW_INSECURE_REMOTE=1`.

Errors are always JSON:

```json
{"error": {"code": "not_found", "message": "no such resource", "status": 404}}
```

## Configuration

| Variable | Flag | Default | Meaning |
| --- | --- | --- | --- |
| `GYGAX_HOST` | `--host` | `127.0.0.1` | Bind address |
| `GYGAX_PORT` | `--port` | `1984` | Listen port (`0` picks a free port) |
| `GYGAX_API_TOKEN` | `--token` | none | Bearer token |
| `GYGAX_API_TOKEN_FILE` | `--token-file` | none | Read the token from a file |
| `GYGAX_ENGINES` | `--engine` (repeatable) | `echo` | Comma-separated engine specs: `echo`, `ollama`, `llama-cpp`, `lmstudio` (each optionally `@host:port`), or `http://host:port/v1[;model=M][;key=K]` |
| `GYGAX_PEERS` | `--peer` (repeatable) | none | Other Gygax nodes, e.g. `http://10.0.0.5:1984` |
| `GYGAX_PEER_TOKEN` | `--peer-token` | the node token | Token presented to peers |
| `GYGAX_MODEL` | `--model` | none | Default model for chat and agents |
| `GYGAX_TOOL_PROTOCOL` | `--tool-protocol` | `json` | Agent tool-calling wire format: `json` or `atari` (see `docs/ATARI.md`) |
| `GYGAX_CORS_ORIGIN` | `--cors-origin` | none | `Access-Control-Allow-Origin` value |
| `GYGAX_RATE_LIMIT_PER_MINUTE` | `--rate-limit` | `0` (off) | Per client IP, fixed one-minute window |
| `GYGAX_HTTP_WORKERS` | `--workers` | `16` | HTTP worker threads |
| `GYGAX_AGENT_WORKERS` | `--agent-workers` | `4` | Agent runtime threads |
| `GYGAX_AGENT_TIMEOUT_MS` | `--agent-timeout-ms` | `120000` | Time limit for an agent chat request |
| `GYGAX_DEVICE_COMMANDS` | `--device-commands` | off | Allow commands to robots, vehicles and buses ([ROBOTICS.md](ROBOTICS.md)) |
| `GYGAX_LEDGER_PATH` | `--ledger` | none | Journal file for the logistics ledger ([LOGISTICS.md](LOGISTICS.md)) |
| `GYGAX_PLUGINS` | `--plugin` (repeatable) | none | Native plugins to load ([EXTENSIONS.md](EXTENSIONS.md)) |
| `GYGAX_EXTENSIONS` | `--extension` (repeatable) | none | Extension processes whose tools to register |
| `GYGAX_MCP` | `--mcp` (repeatable) | none | `NAME=COMMAND ARGS` MCP servers started over stdio; tools appear as `mcp.NAME.TOOL` (see `docs/RESEARCH.md`) |
| `GYGAX_LOG_LEVEL` | `--log-level` | `info` | `trace debug info warn error off` |
| `GYGAX_LOG_FORMAT` | `--log-format` | `text` | `text` or `json` |

Engine specs: `echo`, or `http://host:port/v1[;model=NAME][;key=APIKEY][;peer=1]`. `GYGAX_ENGINE_API_KEY` supplies a default key. Only plain `http://` upstreams are supported by the built-in client; terminate TLS with a local proxy if an engine requires it.

The `echo` engine is a deterministic test backend (it repeats the last user message and understands `!tool <name> <input>` in agent mode). It is what makes `gygax doctor` and CI work without a model. Remove it from production by setting `GYGAX_ENGINES` explicitly.

## Health and observability

| Endpoint | Auth | Purpose |
| --- | --- | --- |
| `GET /healthz` | no | Liveness: `{"status":"ok"}` |
| `GET /readyz` | no | `200` when at least one engine is healthy, otherwise `503` |
| `GET /version` | no | Name and version |
| `GET /metrics` | yes | Prometheus text format |

Metrics: `gygax_build_info`, `gygax_uptime_seconds`, `gygax_http_requests_total`, `gygax_http_responses_total{class}`, `gygax_http_in_flight`, `gygax_http_rejected_overload_total`, `gygax_http_bytes_total{direction}`, `gygax_agents`, `gygax_agent_runs_total{result}`, `gygax_agent_model_calls_total`, `gygax_agent_tool_calls_total`, `gygax_agent_queue_depth`, `gygax_chat_requests_total`, `gygax_chat_errors_total`, and per engine `gygax_engine_healthy`, `gygax_engine_pending`, `gygax_engine_requests_total{result}`, `gygax_engine_latency_ewma_ms`.

## Inference API

### `GET /v1/models`

The standard model-list format (`{"object":"list","data":[...]}`). Contains the union of models advertised by healthy engines plus `gygax-agent`.

### `POST /v1/chat/completions`

The standard chat-completions request and response shapes (`model`, `messages`, `temperature`, `max_tokens`, `stream`). `content` may be a string or an array of `{"type":"text","text":...}` parts. Roles: `system`, `user`, `assistant`, `tool`, `developer` (mapped to `system`).

- `stream: true` returns Server-Sent Events (`chat.completion.chunk` objects terminated by `data: [DONE]`). If the upstream fails before the first byte the client gets a proper JSON error with the right status and the next engine is tried first.
- `model: "gygax-agent"` or `"gygax-agent:<model>"` runs the agent loop and returns its final answer as the assistant message. Streaming is supported but delivers the answer in one chunk.
- Any other model is routed to an engine that advertises it. Engines with an unknown inventory are eligible for any model.

Routing: eligible engines are healthy, enabled, and advertise the model (`:latest` and case are normalised). They are ranked by `pending * 1000 + latency EWMA`, ties broken by id; a pinned engine goes first. Retryable failures (transport errors, 408/425/429/5xx, a 404 from stale inventory) move to the next candidate. Two consecutive retryable failures mark an engine unhealthy until a probe succeeds.

### Ollama compatibility

`GET /api/tags`, `GET /api/version`, `POST /api/chat`, `POST /api/generate`. `stream` defaults to `true` as in Ollama (newline-delimited JSON).

## Agents

Agents are in memory and disappear on restart.

| Method and path | Body | Notes |
| --- | --- | --- |
| `GET /v1/agents` | | List |
| `POST /v1/agents` | `{"name":"x","max_steps":8}` | `201`; `max_steps` is clamped to 1-64 |
| `GET /v1/agents/{id}` | | Status: `idle`, `running`, `completed`, `failed` |
| `DELETE /v1/agents/{id}` | | Cancels a running objective |
| `POST /v1/agents/{id}/objectives` | `{"objective":"...","model":"...","wait_seconds":30}` | `200` when finished, `202` when still running, `409` if busy |
| `POST /v1/agents/{id}/cancel` | | `202` or `409` |
| `GET /v1/agents/{id}/memory?tail=100` | | Episodic memory entries |

The loop: the model receives a system prompt listing the registered tools and the recent memory. It answers with `{"tool":"name","input":"..."}` to call a tool (the result is fed back as `Tool result (name): ...`) or `{"answer":"..."}` (or plain text) to finish. Tool errors and unknown tools are reported to the model, not raised. Runs stop at `max_steps` with `error: "step limit reached"`.

## Tools

`GET /v1/tools` lists them; `POST /v1/tools/{name}/invoke` with `{"input": "..."}` (or an object) runs one. Built in:

| Tool | Input | Output |
| --- | --- | --- |
| `math.eval` | `2 + 3 * (4 - 1) ^ 2` | `+ - * / % ^`, parentheses, `sqrt sin cos tan exp ln log10 abs floor ceil round min max pow`, `pi`, `e` |
| `time.now` | ignored | UTC ISO-8601 |
| `node.info` | ignored | CPU, memory, load, GPUs (via `nvidia-smi` when present) |
| `neuro.run` | network spec JSON | spike statistics, see [NEUROMORPHIC.md](NEUROMORPHIC.md) |
| `webpage.construct` | ignored | builds the Mother/Father/Child portal page used by the Chrome extension |
| `device.list`, `device.state` | id | Device inventory and telemetry |
| `device.command` | `{"device","command",...}` | Only registered when device commands are enabled |
| `logistics.record`, `logistics.summary`, `logistics.plan`, `logistics.units` | line or JSON | Inventory flow and route planning |
| `plugin.<name>.<tool>`, `ext.<name>.<tool>` | tool specific | Tools from plugins and extension processes ([EXTENSIONS.md](EXTENSIONS.md)) |

Register your own from C++: `tools::ToolRegistry::getInstance().registerTool("name", fn, "description")`.

## Engines and cluster

| Method and path | Purpose |
| --- | --- |
| `GET /v1/engines` | Health, inventory, counters, latency |
| `POST /v1/engines` | `{"id":"gpu1","spec":"http://10.0.0.7:11434/v1"}` |
| `DELETE /v1/engines/{id}` | Remove |
| `POST /v1/engines/{id}/enable` / `disable` | Take out of rotation |
| `POST /v1/pin` | `{"id":"gpu1"}` or `{}` to clear |
| `POST /v1/probe` | Probe all engines now |
| `GET /v1/node` | Hardware and load of this node |
| `GET /v1/cluster` | This node plus peer engine status |

Peers are ordinary Gygax nodes registered as remote engines. Requests forwarded between nodes carry `X-Gygax-Forwarded: 1`; a node serving such a request only routes to its local engines, hides remote models from `/v1/models` and rejects `gygax-agent`, so peers can never form a routing loop. Discovery is by configuration; there is no multicast.

## Devices

Robots, vehicles, buses and instruments, see [ROBOTICS.md](ROBOTICS.md).

| Method and path | Purpose |
| --- | --- |
| `GET /v1/devices` | Devices, supported kinds, whether commands are enabled |
| `POST /v1/devices` | `{"id":"uav","kind":"mavlink","uri":"udp://0.0.0.0:14550"}` |
| `DELETE /v1/devices/{id}` | Disconnect and remove |
| `GET /v1/devices/{id}/state` | Latest telemetry |
| `POST /v1/devices/{id}/command` | `{"command":"arm"}`; `403` unless commands are enabled |

## Logistics

Ledger, units, sites and range-aware planning under `/v1/logistics`, see [LOGISTICS.md](LOGISTICS.md).

## JSON-RPC 2.0

`POST /rpc` (and `gygax rpc` over stdin/stdout, one JSON document per line). Single requests and batches are supported. Methods mirror the REST operations: `node.info`, `cluster.info`, `engines.list|add|remove|enable|disable|pin|probe`, `tools.list|invoke`, `agents.list|create|get|delete|submit|cancel|memory`, `devices.list|add|remove|state|command`, `logistics.*`, `neuro.simulate`, `chat.complete`. HTTP-level failures map to error code `-32000` with `data.status`.

## Limits

Request bodies up to 8 MiB, headers up to 16 KiB, 10 s read timeout, 30 s write timeout, 256 queued connections (further connections receive `503`), 2000 messages per chat request, 64 KiB per agent objective, 200,000 neurons per simulation request.

## Legacy portal endpoints

`POST /directive` and `GET /status` keep the bundled Chrome extension working. They require the token when one is configured.
