# Operations guide

## Install

### Debian or Ubuntu package

```bash
cmake --preset release && cmake --build --preset release
cpack --config build/release/CPackConfig.cmake -G DEB
sudo apt install ./gygax_*.deb
sudo editor /etc/gygax/gygax.env          # set GYGAX_API_TOKEN and GYGAX_ENGINES
sudo systemctl enable --now gygax
```

The package creates a `gygax` system user, installs `/etc/gygax/gygax.env` (mode 0640, only if missing) and the hardened unit from `deploy/gygax.service`. Upgrades keep your env file.

### Tarball

`cpack -G TGZ` produces a relocatable archive with `bin/gygax`, `lib/libgygax_c.so`, headers, the Python package and docs. Run `gygax serve` under any supervisor.

Versioned SDK packages and a source archive are attached to GitHub Releases.
The release workflow tests the installed plugin SDK consumer before publishing;
see [Releasing Gygax](RELEASING.md) for supported artifacts and checksums.

### Container

```bash
docker build -t gygax .
docker run -d --name gygax -p 1984:1984 -e GYGAX_API_TOKEN=... \
  -e GYGAX_ENGINES=http://host.docker.internal:11434/v1 gygax
```

The image runs as UID 10001, logs JSON to stdout and has a `/healthz` health check. `deploy/docker-compose.yml` runs Gygax next to Ollama with a read-only root filesystem and all capabilities dropped.

### From source

```bash
./setup.sh && ./build.sh && sudo cmake --install build
```

## Sizing and tuning

- `GYGAX_HTTP_WORKERS` (default 16): concurrent HTTP requests. Streaming chat holds a worker for the duration of the stream, so size it above your expected concurrent streams. Excess connections queue (256) and then receive `503`.
- `GYGAX_AGENT_WORKERS` (default 4): concurrent agent objectives; each holds a worker while it waits for the model.
- The service itself is light: with the echo engine one core sustains several thousand requests per second (measured about 3,000 req/s with a Python client on one machine; p50 4 ms). Real capacity is set by your engines.
- Agent state is memory only. Restarting the service forgets agents and their memory; clients should treat agents as ephemeral.

## TLS

The service speaks plain HTTP. Put a TLS terminator in front when traffic leaves the host. Caddy:

```
gygax.example.com {
    reverse_proxy 127.0.0.1:1984 {
        flush_interval -1
    }
}
```

nginx needs `proxy_buffering off;` and `proxy_http_version 1.1;` for streaming responses. Keep `GYGAX_HOST=127.0.0.1` behind the proxy and keep the bearer token enabled; it is your authentication.

## Multi-node routing

Run one node per GPU host and one front node:

```bash
gygax serve --host 0.0.0.0 --token-file /etc/gygax/token --engine http://127.0.0.1:11434/v1        # on each GPU host
gygax serve --peer http://gpu1:1984 --peer http://gpu2:1984 --peer-token "$TOKEN" --token-file ...  # front node
```

`GET /v1/cluster` on the front node shows each peer's health, models and latency. Peers are probed every 5 s (15 s while unhealthy).

## Monitoring

```yaml
scrape_configs:
  - job_name: gygax
    authorization: { credentials_file: /etc/prometheus/gygax-token }
    static_configs: [{ targets: ["gygax.example.com:1984"] }]
```

Useful alerts: `gygax_engine_healthy == 0`, `rate(gygax_chat_errors_total[5m]) / rate(gygax_chat_requests_total[5m]) > 0.05`, `gygax_agent_queue_depth > 20`, `rate(gygax_http_rejected_overload_total[5m]) > 0`, `rate(gygax_http_responses_total{class="5xx"}[5m]) > 0`.

Logs go to stderr (`journalctl -u gygax`). Set `GYGAX_LOG_FORMAT=json` for structured logs and `GYGAX_LOG_LEVEL=debug` to see one line per request.

## Security checklist

- Set a token of at least 24 random bytes; prefer `GYGAX_API_TOKEN_FILE` over command-line flags (flags are visible in `ps`).
- Bind to loopback and publish through a TLS proxy, or set `GYGAX_RATE_LIMIT_PER_MINUTE`.
- The engine-management endpoints (`POST /v1/engines`) let a token holder make the node connect to arbitrary `http://` URLs. Treat the token as an administrator credential.
- The `neuro.run` tool and `/v1/neuro/simulate` accept user-supplied networks, capped at 200,000 neurons and 10 minutes of simulated time per call.
- The systemd unit drops all capabilities, makes the filesystem read-only, blocks address families other than IP and Unix, and forbids executable memory mappings.
- Report vulnerabilities as described in [SECURITY.md](../SECURITY.md).

## Verification and diagnostics

```bash
gygax doctor                                       # in-process self test
gygax doctor --url http://host:1984 --token ... --full   # smoke test a running node
gygax status --url http://host:1984 --token ...    # node, engines, agents
scripts/collect-diagnostics.sh                     # redacted support bundle
```

`gygax doctor --url` without `--full` only performs read-only checks. `--full` creates and deletes an agent and sends a few chat requests to the `echo` engine.

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `refusing to listen on '0.0.0.0' without GYGAX_API_TOKEN` | Set a token, or `GYGAX_ALLOW_INSECURE_REMOTE=1` on an isolated network |
| `/readyz` returns 503 | No engine is healthy. `GET /v1/engines` shows `last_error` per engine |
| `503 no healthy engine advertises model 'x'` | The model is not in any engine's `/v1/models`. Pull it or fix the name; `:latest` and case are ignored |
| `504 agent did not finish before the timeout` | The engine is slow; raise `GYGAX_AGENT_TIMEOUT_MS` or shorten the objective |
| Streaming responses arrive all at once | A proxy is buffering; disable buffering for `/v1/chat/completions` |
| `cannot connect ... Connection refused` for an https engine | The built-in client is HTTP only; front the engine with a local TLS-terminating proxy |
| High `gygax_http_rejected_overload_total` | Raise `GYGAX_HTTP_WORKERS` or add nodes |
| Slow shutdown | In-flight model calls finish before exit (up to the engine read timeout of 120 s); the unit allows 150 s |
