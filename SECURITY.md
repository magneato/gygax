# Security policy

## Reporting a vulnerability

Please do not open public issues for security problems. Email robert.sitton@neuralsplines.com with:

- the affected version or commit (`gygax version`),
- the vulnerability class and impact,
- reproduction steps or a proof of concept.

Remove tokens, prompts and model data from reports. You will get an acknowledgement within a few days.

## Security model

Gygax is designed to run on a trusted host or LAN behind a TLS terminator.

- **Authentication**: a single bearer token protects every endpoint except `/healthz`, `/readyz` and `/version`. It is compared in constant time. Without a token the service only binds to loopback unless `GYGAX_ALLOW_INSECURE_REMOTE=1` is set.
- **Authorization**: the token is an administrator credential. Holders can add engines (the node then connects to those `http://` URLs), invoke tools and run simulations. There are no per-user roles.
- **Transport**: plain HTTP only. There is no built-in TLS, and the HTTP client refuses `https://` URLs instead of pretending. Terminate TLS in a proxy; peers should talk over a private network or a TLS tunnel.
- **Input handling**: bounded request line, headers (16 KiB) and bodies (8 MiB), strict header parsing (no obsolete folding, conflicting length headers rejected), JSON depth limit 64, timeouts on every read and write, bounded connection queue, optional per-IP rate limiting.
- **Tools**: built-in tools have no shell, filesystem or network access. `math.eval` is a recursive-descent evaluator, not `eval`. Tools you register run with the service's privileges; treat model-produced tool input as untrusted.
- **Devices**: commands to robots, vehicles and buses are off unless `--device-commands` is given, and then the `device.command` tool becomes available to agents as well. Reading telemetry is always allowed. Serial paths must be under `/dev/`, device ids are validated, at most 64 devices. Gygax adds no safety layer to the machine itself; keep the vehicle's own failsafes and a hardware kill switch. See [docs/ROBOTICS.md](docs/ROBOTICS.md).
- **Plugins and extensions**: a native plugin runs inside the service with its privileges; the loader refuses files owned by other users or writable by others, which prevents accidents but is not a sandbox. Extension processes are isolated but trusted for their tool output. Both are configured at startup only. See [docs/EXTENSIONS.md](docs/EXTENSIONS.md).
- **Logistics ledger**: journal file is mode 0600 and `fsync`ed; it records what API clients and agents report, so protect the token. See [docs/LOGISTICS.md](docs/LOGISTICS.md).
- **Isolation**: the shipped systemd unit and container drop all capabilities, restrict address families and make the filesystem read-only.
- **Verification**: the test suite runs under AddressSanitizer, UBSan and ThreadSanitizer.

## Out of scope

Denial of service by an authenticated administrator, and attacks that require control of the host or of the upstream inference engine.
