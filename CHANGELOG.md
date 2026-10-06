# Changelog

## 0.2.0

### Added
- `gygax` CLI with `serve`, `doctor`, `rpc`, `neuro`, `status`, `ask`.
- HTTP service: chat over the standard `/v1/chat/completions` API and the Ollama API, model-aware routing with failover, health probing, peer nodes, agent runtime with tools and memory, Prometheus metrics, JSON-RPC (HTTP and stdio), bearer authentication, rate limiting, graceful shutdown.
- Neuromorphic simulator (LIF, Izhikevich, Poisson, spike source, delays, STDP), JSON spec, C ABI, Python package and HTTP client, `NeuromorphicNode`.
- 2D simulation world with LiDAR, UDP/TCP robot link, `.usda` export, rewritten Arrival example with a spiking controller.
- Real serial, GPIO, Hayes modem, printer and Marlin G-code transports.
- JSON library, structured logger, safe expression evaluator, node information.
- 290 tests, `gygax doctor` (20 checks), CTest entries for Arrival, Python and the retro toolchain, sanitizer presets, `scripts/dogfood.sh`, `scripts/lint.sh`.
- Robotics stack: device registry and `/v1/devices` API, MAVLink v1/v2, SocketCAN, ISO-TP, OBD-II, J1939, DBC, CANopen, Modbus, NMEA 0183, ADS-B, ARINC 429, rosbridge, and a virtual autopilot (`gygax sim-autopilot`). Commands are disabled unless `--device-commands` is given. See `docs/ROBOTICS.md`.
- Supply chain logistics: GUID-tracked units, append-only ledger with durable journal and the compact `-3 ba99x drone=quadcopter power=solar` line format, flow summaries, sites, range-aware route planning with refuel stops and refuel-point suggestions, live fuel and position from bound devices, agent tools, metrics. See `docs/LOGISTICS.md`.
- SDKs: native plugin SDK (C ABI, C++ helper, exported CMake package with `gygax_add_plugin`), extension-process protocol with a Python SDK, `gygax.LocalService`, Python client for devices and logistics; the Python package is staged by the normal build. See `docs/EXTENSIONS.md`.
- ATARI: an opt-in, compact alternative to the agent tool-calling protocol's default JSON (`gygax/inference/atari.hpp`, `--tool-protocol atari` / `GYGAX_TOOL_PROTOCOL`). Interns tool names into short codes for the run (a prefix-tree schema cache) so `CALL`/`RESULT`/`ERROR` frames stay a few characters each; `EchoBackend` speaks both protocols, so it works end to end with no real model. See `docs/ATARI.md`.
- `gyde`: a terminal preview of a planned Gygax IDE (`docs/GYDE.md`): an always-current status line and typed commands (`status`, `engines`, `ask`, `tool`) against a running node, installed alongside `gygax`.
- Windows 11 pipeline via Docker Desktop/WSL2: a `dev` stage in the `Dockerfile` carries the toolchain with the source bind-mounted at run time, and every `.sh` gets a `.ps1`/`.bat` that runs the identical script inside it (`setup`, `build`, `assemble`, `scripts/{dogfood,lint,sdk-check,collect-diagnostics,docker-build}`); `scripts/devshell.sh` gives the same container shell on Linux/macOS. Best-effort Windows CI job. See `docs/WINDOWS.md`.
- `wargames` showcase: agent-commanded last stand against an alien invasion, directable by a human through an interactive console or a timed script; `--engine` can hand decisions to a real `ollama`, `llama-cpp` or `lmstudio` model instead of the built-in doctrine, and `--mcp-serve` exposes its tools over MCP. See `docs/WARGAMES.md`.
- Research toolkit: TF2-style `TfBuffer` with SE(3) math, URDF parser with forward/inverse kinematics, control barrier function velocity shield and joint limit shield, action-chunk temporal ensembling, a deterministic vectorized rover environment with domain randomization exposed to Python as `gygax.rl` (NumPy `VecEnv`, Gymnasium-style `GygaxRoverEnv`), and EVT2 event camera decoding with spiking motor decoders. See `docs/RESEARCH.md`.
- MCP client (`--mcp`) and server (`serveMcp`) for the Model Context Protocol, and `ollama`, `llama-cpp` and `lmstudio` engine presets alongside plain `http://host:port/v1` engines. See `docs/RESEARCH.md`.
- Eigen and GMP behind swappable `math::LinearBackend` and `math::IntegerBackend` interfaces (dense linear algebra and arbitrary-precision integers), so the library never depends on either directly. Building now needs `libeigen3-dev` and `libgmp-dev`.
- Packaging: install rules, DEB and tar packages, Dockerfile, compose file, hardened systemd unit, diagnostics collector.

### Changed
- Event relay is publish/subscribe with bounded per-subscriber queues (previously a single shared queue that consumers raced on).
- World model access is snapshot/mutate under a lock (previously raw shared pointers).
- Tool registry runs tools outside its lock.
- Message hub delivers buffered messages to late registrants and allows re-entrant sends.
- Embodiments hold real state and integrate it; workers wait on a condition variable instead of spinning.
- Console chatter in the core replaced by leveled logging.
- CMake: options for tests, examples, toys, sanitizers, `-Werror`; presets for dev, release, asan, tsan.

### Removed
- Placeholder transports (fax, scanners, QR, infrared, JTAG, multiplexer), the mocked Isaac Sim, ROS and USD/Hydra bridges, the stubbed GANS and agent HTTP(S)/FTP(S) protocols (they only logged), the fake reflection module, the raylib games, and stale status documents. See `trash/README.md`.
- Windows as a native target.
