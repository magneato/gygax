# Changelog

## 0.3.0 "Skynet" (2026-10-09)

Release notes in [docs/releases/v0.3.0.md](docs/releases/v0.3.0.md). Includes everything in 0.2.0-alpha.1 "Genesys" and the 0.2.0 work below it.

### Added
- **SatLink**, satellite tracking for ground stations (`include/gygax/satlink/satlink.hpp`, [docs/SATLINK.md](docs/SATLINK.md)):
  - Two-line element parsing with checksum verification, Alpha-5 catalog numbers, title lines, and errors that name the line, columns and field.
  - SGP4 and SDP4 (lunar-solar terms, 12- and 24-hour resonances) after Vallado et al. 2006. All 33 satellites of the reference verification set match `tcppver.out` to 0.12 mm and 0.0009 mm/s; propagation is a pure, thread-safe function of time.
  - TEME to Earth-fixed and geodetic frames, look angles, exact range rate and first-order Doppler; agrees with Skyfield to 0.0003° in elevation and 2.5 m in range.
  - Pass prediction: rise and set to 10 ms, culmination to 0.1 s, clipped passes marked, geostationary satellites handled.
  - Hamlib `rigctld` client that keeps one connection across retunes (`F`, `f`, `RPRT` checking, reconnect, timeouts).
  - Software-defined radio (`include/gygax/satlink/sdr.hpp`): an `rtl_tcp` client (greeting check, tuning, sample rate, gain, ppm, bias tee, IQ capture), an averaged Hann-windowed FFT power spectrum, sub-bin peak estimation with SNR, and a phase-continuous mixer that removes a sweeping Doppler shift; `satlink.RtlTcp` in Python. Tested against a simulated sky that plays the ISS's Doppler-shifted carrier.
  - `gygax track`: pass tables with Doppler, or live look angles once a second with optional rig (`--rig`) and SDR (`--sdr`) retuning; with an SDR it reports the measured carrier offset and SNR.
  - C ABI (`gygax_satlink_*`), Python package `gygax.satlink` with a Skyfield-shaped subset (`EarthSatellite`, `wgs84.latlon`, `(sat - station).at(t).altaz()`, `find_events`) and `satlink` as an alias, and `examples/satlink`.
  - A `gygax doctor` check against the reference vectors (21 checks), and CTest entries for the CLI, the Python example and OHFLOCK (404 in all).
- **OHFLOCK** showcase (`examples/ohflock`, [docs/OHFLOCK.md](docs/OHFLOCK.md)): twenty-four unarmed kites hold a relief corridor open against fighters, a SAM battery, acoustic-cued AAA and strike jets, with low-power lasers on sensors only, thrown sound, radar notching and predicted break turns, and SatLink: maximum-a-posteriori LEO Doppler navigation (position and inertial bias) under GNSS jamming, relay command windows, and a reconnaissance pass predicted and waited out. The flock fires nothing. A self-contained 3D replay (`--html`), live direction from gyde through `flock.*` tools (`--serve`), scripted orders, a `--no-satlink` counterfactual, and `--selfcheck` (a ctest entry).
- `gygax version` prints the release name (`gygax 0.3.0 "Skynet"`); `GYGAX_VERSION_PRERELEASE` and `GYGAX_VERSION_CODENAME` in `gygax/version.hpp`.

### Changed
- Version 0.3.0. Versions are consistent everywhere (CLI, C ABI, service, packages), and the Python package uses the PEP 440 form (`gygax.version_pep440()`, checked by a test). Pre-release labels must be `alpha.N`, `beta.N` or `rc.N`, mapping to PEP 440 (`a1`) and Debian (`~alpha.1`, sorting before the release) versions; package files carry the full version.
- `build.sh` prefers the newest installed clang (21 down to 18).
- The release workflow checks the whole tag, pre-release label included, against `CMakeLists.txt`.

### Fixed
- The project page no longer scrolls sideways on phones.

## 0.2.0-alpha.1 "Genesys" (2026-10-06)

First public pre-release of 0.2.0; release notes in [docs/releases/v0.2.0-alpha.1.md](docs/releases/v0.2.0-alpha.1.md).

### Fixed
- A crashed MCP server no longer kills the service: writes to its pipes block SIGPIPE in the calling thread and report "mcp server exited".
- macOS (Homebrew LLVM) builds and passes the full test suite: portable floating-point parsing (`gygax::fromChars`), `EREMOTEIO` and close-on-exec fallbacks, `getentropy`, node information from `sysctl`/Mach, GMP and Eigen 3.3 to 5.x found by CMake, and the messaging module's `Message` attached to the global module as the standard requires.
- Timing-sensitive tests made reliable (gyde startup wait, WebSocket handshake count, wargames engine parity).
- GitHub Pages publishes (`.nojekyll`); social preview image.

### Changed
- CI: the macOS job uses Homebrew LLVM; the Windows job checks every PowerShell script parses (hosted runners cannot run the Docker pipeline, which the Linux package job covers).
- Release workflow accepts pre-release tags such as `v0.2.0-alpha.1` and takes notes from `docs/releases/`.

## 0.2.0 development (first published as 0.2.0-alpha.1; released in 0.3.0)

### Added
- `gygax` CLI with `serve`, `doctor`, `rpc`, `neuro`, `status`, `ask`.
- HTTP service: chat over the standard `/v1/chat/completions` API and the Ollama API, model-aware routing with failover, health probing, peer nodes, agent runtime with tools and memory, Prometheus metrics, JSON-RPC (HTTP and stdio), bearer authentication, rate limiting, graceful shutdown.
- Neuromorphic simulator (LIF, Izhikevich, Poisson, spike source, delays, STDP), JSON spec, C ABI, Python package and HTTP client, `NeuromorphicNode`.
- 2D simulation world with LiDAR, UDP/TCP robot link, `.usda` export, rewritten Arrival example with a spiking controller.
- Real serial, GPIO, Hayes modem, printer and Marlin G-code transports.
- JSON library, structured logger, safe expression evaluator, node information.
- 376 CTest entries, `gygax doctor` (20 checks), CTest entries for Arrival, Python and the retro toolchain, sanitizer presets, `scripts/dogfood.sh`, `scripts/lint.sh`.
- Robotics stack: device registry and `/v1/devices` API, MAVLink v1/v2, SocketCAN, ISO-TP, OBD-II, J1939, DBC, CANopen, Modbus, NMEA 0183, ADS-B, ARINC 429, rosbridge, and a virtual autopilot (`gygax sim-autopilot`). Commands are disabled unless `--device-commands` is given. See `docs/ROBOTICS.md`.
- Supply chain logistics: GUID-tracked units, append-only ledger with durable journal and the compact `-3 ba99x drone=quadcopter power=solar` line format, flow summaries, sites, range-aware route planning with refuel stops and refuel-point suggestions, live fuel and position from bound devices, agent tools, metrics. See `docs/LOGISTICS.md`.
- SDKs: native plugin SDK (C ABI, C++ helper, exported CMake package with `gygax_add_plugin`), extension-process protocol with a Python SDK, `gygax.LocalService`, Python client for devices and logistics; the Python package is staged by the normal build. See `docs/EXTENSIONS.md`.
- ATARI: an opt-in, compact alternative to the agent tool-calling protocol's default JSON (`gygax/inference/atari.hpp`, `--tool-protocol atari` / `GYGAX_TOOL_PROTOCOL`). Interns tool names into short codes for the run (a prefix-tree schema cache) so `CALL`/`RESULT`/`ERROR` frames stay a few characters each; `EchoBackend` speaks both protocols, so it works end to end with no real model. See `docs/ATARI.md`.
- `gyde`: a terminal preview of a planned Gygax IDE (`docs/GYDE.md`): an always-current status line and typed commands (`status`, `engines`, `ask`, `tool`) against a running node, installed alongside `gygax`.
- Windows 11 pipeline via Docker Desktop/WSL2: a `dev` stage in the `Dockerfile` carries the toolchain with the source bind-mounted at run time, and every `.sh` gets a `.ps1`/`.bat` that runs the identical script inside it (`setup`, `build`, `assemble`, `scripts/{dogfood,lint,sdk-check,collect-diagnostics,docker-build}`); `scripts/devshell.sh` gives the same container shell on Linux/macOS. See `docs/WINDOWS.md`.
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
