# Gygax

Gygax is a C++26 runtime for autonomous systems. It ships as four things that share one core:

- **A service** (`gygax serve`): an inference router that speaks the standard chat-completions API and the Ollama API, with model-aware routing, failover and peer nodes, plus a tool-using agent runtime, Prometheus metrics and JSON-RPC. One static-ish binary, no Python, Go or Node runtime.
- **A neuromorphic simulator**: deterministic LIF and Izhikevich spiking networks with delayed sparse projections and pair-based STDP, usable from C++, JSON, HTTP, a C ABI and Python.
- **An embodiment toolkit**: hardware node trees with capability aggregation, hierarchical state machines, a message hub, a 2D physics world with LiDAR, a sim-to-real link, USD export, and real serial, GPIO, modem, printer and G-code transports.
- **SatLink**: satellite tracking for ground stations. SGP4/SDP4 that reproduces the reference implementation to a tenth of a millimetre, look angles, exact range-rate Doppler, pass prediction, radio retuning through Hamlib `rigctld`, and reception with an RTL-SDR over `rtl_tcp`.

Everything documented here is exercised by the test suite (404 CTest entries), the built-in `gygax doctor`, and the sanitizer builds.

**New in 0.3.0 "Skynet":** SatLink, with SDR reception. See the [release notes](docs/releases/v0.3.0.md) and the [changelog](CHANGELOG.md).

## Quick start: run it as a service

```bash
git clone https://github.com/magneato/gygax && cd gygax
./setup.sh                      # checks clang-18, cmake >= 3.28, ninja
./build.sh                      # builds into ./build
./build/gygax doctor            # 21 self-tests, in-process, no network needed
```

On Windows 11, run the same commands as `.\setup.ps1`, `.\build.ps1` and `.\build\gygax doctor` through Docker Desktop (WSL2 backend). No native Windows toolchain is needed; see [docs/WINDOWS.md](docs/WINDOWS.md).

Serve with the built-in deterministic `echo` engine, or point it at any engine you run that speaks the standard chat-completions API (Ollama, LM Studio, llama.cpp, vLLM, another Gygax node):

```bash
export GYGAX_API_TOKEN="$(openssl rand -hex 24)"
./build/gygax serve --engine http://127.0.0.1:11434/v1 --model llama3.1

curl -s localhost:1984/v1/chat/completions \
  -H "Authorization: Bearer $GYGAX_API_TOKEN" -H 'Content-Type: application/json' \
  -d '{"model":"llama3.1","messages":[{"role":"user","content":"hello"}]}'
```

The model name `gygax-agent` runs the full agent loop (tools, memory, step limit) behind the same endpoint:

```bash
curl -s localhost:1984/v1/chat/completions -H "Authorization: Bearer $GYGAX_API_TOKEN" \
  -d '{"model":"gygax-agent:llama3.1","messages":[{"role":"user","content":"What is sqrt(2)*sqrt(2)?"}]}'
```

Production install (Debian/Ubuntu package, systemd unit, TLS in front, Prometheus): see [docs/OPERATIONS.md](docs/OPERATIONS.md). Containers: `docker build -t gygax .` or `deploy/docker-compose.yml`.

Defaults are safe: the service binds to `127.0.0.1`, refuses to bind elsewhere without a token, limits body sizes and timeouts, compares tokens in constant time, and never logs secrets.

## Your hardware, your models, your cloud

Gygax runs where you put it and talks only to what you point it at.

- **No telemetry.** There are no analytics, usage reports, update checks or license servers. Nothing phones home.
- **No built-in cloud.** Gygax has no hosted service and no default model provider. The built-in `echo` engine works with no network at all; every other engine is one you name with `--engine`.
- **Outbound connections are yours.** Gygax connects only to the engines, peer nodes, MCP servers and devices (serial, CAN, MAVLink and so on) that you configure.
- **Local by default.** The service listens on `127.0.0.1` and will not listen on any other address until you set an API token.
- **Builds offline.** Configure with `-DGYGAX_BUILD_TESTS=OFF` and the build needs nothing from the internet. With tests on, CMake downloads GoogleTest once.
- **The browser extension stays home.** Its only permission is `http://127.0.0.1:1984`.
- **Check it yourself.** It is all source. Read `src/gygax/net` and `src/gygax/service`, run it behind a firewall that blocks everything but your engines, or watch it with `tcpdump`.

## Quick start: neuromorphic research

```bash
./build.sh
export GYGAX_LIB=$PWD/build/libgygax_c.so PYTHONPATH=$PWD/python
python3 examples/python/stdp_demo.py
```

```python
from gygax import neuro

with neuro.Network(dt_ms=0.1, seed=7) as net:
    stimulus = net.add_poisson("stimulus", 100, rate_hz=30)
    cortex = net.add_lif("cortex", 200)
    plastic = neuro.Stdp(a_plus=0.01, a_minus=0.0105, w_min=0, w_max=12)
    proj = net.connect(stimulus, cortex, p=0.2, weight=6.0, weight_std=1.0, stdp=plastic)
    net.run(2000)
    print(cortex.mean_rate_hz, proj.mean_weight)
    times_ms, neuron_ids = cortex.spikes()
```

The same network from the shell (`gygax neuro spec.json`), over HTTP (`POST /v1/neuro/simulate`), or as an agent tool (`neuro.run`). A 10,000-neuron, 2.3M-synapse network simulates 1 s of biological time in about 1.4 s on one core. Model equations, units, determinism guarantees and validation are in [docs/NEUROMORPHIC.md](docs/NEUROMORPHIC.md).

## Quick start: satellite tracking

```
$ ./build/gygax track --tle examples/satlink/sample.tle --at 51.4779,-0.0015,46 --start 2024-01-01T12:00 --downlink 437.8e6
...
  rise (UTC)            from       peak (UTC)  max el   set (UTC)   to         lasts    doppler rise → set
  2024-01-01 13:22:56   219.5° SW   13:26:02     35.2°  13:29:08    84.7° E     6m11s    +9.21 kHz → -9.22 kHz
  2024-01-01 14:59:15   260.2° W    15:02:37     87.7°  15:05:58    82.8° E     6m43s    +9.93 kHz → -9.93 kHz
```

```python
from gygax import satlink

iss = satlink.Satellite(line1, line2, "ISS (ZARYA)")
home = satlink.Observer(51.4779, -0.0015, 46)
look = iss.observe(home, satlink.timescale.now())          # elevation, azimuth, range, range rate
with satlink.Rig("127.0.0.1", 4532) as rig:                 # Hamlib rigctld
    rig.set_frequency(437.8e6 + look.doppler_shift_hz(437.8e6))
```

`gygax track --live --rig 127.0.0.1` follows a pass and retunes the radio every second; `--sdr 127.0.0.1:1234` does the same for an RTL-SDR and reports where the carrier actually is. Scripts written for Skyfield's satellite API (`EarthSatellite`, `wgs84.latlon`, `(sat - station).at(t).altaz()`, `find_events`) run against SatLink unchanged. Propagation is checked against all 33 satellites of Vallado's SGP4 verification set (worst difference 0.12 mm) and against Skyfield (0.0003° in elevation). Frames, Doppler, pass search, accuracy and limits: [docs/SATLINK.md](docs/SATLINK.md).

## What is in the box

| Area | What you get | Where |
| --- | --- | --- |
| Inference routing | Standard `/v1/chat/completions` (JSON and SSE), `/v1/models`; Ollama `/api/chat`, `/api/generate`, `/api/tags`; routing by advertised model, load and latency EWMA; automatic failover; health probing; pinning; peer nodes | `src/gygax/inference`, `src/gygax/service` |
| Agent runtime | Objective loop with tool calls, episodic memory, step limits, cancellation, bounded worker pool, event bus; JSON or the compact ATARI wire protocol (`--tool-protocol`) | `src/gygax/brain/orchestrator.cppm`, [docs/ATARI.md](docs/ATARI.md) |
| Tools | `math.eval`, `time.now`, `node.info`, `neuro.run`, `webpage.construct`, plus your own via `ToolRegistry` | `src/gygax/skills`, `src/gygax/service` |
| Operations | `/healthz`, `/readyz`, `/metrics`, bearer auth, rate limiting, JSON logs, graceful shutdown, JSON-RPC over HTTP and stdio | `docs/OPERATIONS.md` |
| Neuromorphic | LIF, Izhikevich, Poisson and spike-source populations, delays, STDP, seedable RNG, C ABI, Python | `include/gygax/neuro`, `python/` |
| SatLink | Checksum-verified TLE parsing, SGP4/SDP4 (Vallado 2006, verified against `tcppver.out`), TEME, Earth-fixed and geodetic frames, look angles, exact range rate and Doppler, pass prediction, Hamlib `rigctld` control, rtl_tcp SDR client with FFT spectrum and Doppler mixer; C++, C ABI, Python (with a Skyfield-shaped subset), `gygax track` | `include/gygax/satlink`, `src/gygax/satlink`, `python/gygax/satlink`, [docs/SATLINK.md](docs/SATLINK.md) |
| Embodiment | Node hierarchy, capabilities, state machines, message hub with offline buffering, embodiment taxonomy with live telemetry | `src/gygax/hardware`, `src/gygax/brain` |
| Simulation | Differential-drive world, collisions, LiDAR, UDP/TCP robot link, `.usda` export with trajectories | `include/gygax/sim`, `examples/arrival` |
| Acoustic study | In-tree simulation-only coherent tone estimates at defined points and synthetic amplitude-modulated tone samples; not currently part of the installed SDK and has no audio output or hardware control | [`include/gygax/sim/acoustics.hpp`](include/gygax/sim/acoustics.hpp), [model limits and example](docs/ACOUSTICS.md) |
| Quantum adapters | Vendor-neutral navigation-estimate and QKD authenticated-channel interfaces; no device drivers or cryptographic implementation included | [`include/gygax/quantum/adapters.hpp`](include/gygax/quantum/adapters.hpp), [integration boundary](docs/QUANTUM_INTEGRATION.md) |
| Seek & Find demo | Standalone fictional communications, mock quantum-navigation estimate, QKD basis-sifting illustration, and UV-visible prop-search replay; no key material or hardware control | [`examples/seek_and_find`](examples/seek_and_find/README.md) |
| Transports | POSIX serial, sysfs GPIO, Hayes modem, raw/PJL printing, Marlin G-code with line numbers, checksums and resend | `include/gygax/transport`, [docs/TRANSPORTS.md](docs/TRANSPORTS.md) |
| Robotics | MAVLink drones and vehicles, SocketCAN/CAN FD, ISO-TP, OBD-II, J1939, DBC, CANopen, Modbus, NMEA, ADS-B, ARINC 429, ROS bridge, virtual autopilot; commands off by default | `src/gygax/robotics`, `src/gygax/bus`, [docs/ROBOTICS.md](docs/ROBOTICS.md) |
| Supply chain | GUID-tracked units, append-only ledger (`-3 ba99x drone=quadcopter power=solar`), flow reports, range-aware routing with refuel stops | `src/gygax/logistics`, [docs/LOGISTICS.md](docs/LOGISTICS.md) |
| Showcase | `ohflock`: twenty-four unarmed kites hold a relief corridor open against fighters, a SAM battery, AAA and strike jets with light on sensors, thrown sound and SatLink (Doppler navigation under GNSS jamming, relay windows, a reconnaissance pass waited out), and fire nothing; a 3D replay, gyde control, and a `--no-satlink` counterfactual | `examples/ohflock`, [docs/OHFLOCK.md](docs/OHFLOCK.md) |
| Showcase | `wargames`: Gygax agents defend a last bunker against an alien invasion with drones, tanks, turrets and supply drones, using the agent runtime, collective consensus, the ledger, route planner and a spiking hive; a human can direct them live | `examples/wargames`, [docs/WARGAMES.md](docs/WARGAMES.md) |
| SDKs | Native plugin SDK (C ABI, C++ helper, CMake package), Python extension SDK, Python bindings and client, all built by `cmake --build` | `include/gygax/sdk`, `python/`, [docs/EXTENSIONS.md](docs/EXTENSIONS.md), [public API and trust boundaries](docs/SDK_API.md) |
| Retro toolchain | 6502/6809 assembler and a CoCo-style emulator (`./assemble.sh`) | `tools/`, `examples/coco` |
| Gyde | Terminal preview of a planned Gygax IDE: an always-current status line plus typed commands (`status`, `engines`, `ask`, `tool`) against a running node | `tools/gyde.cpp`, [docs/GYDE.md](docs/GYDE.md) |

## Verify everything

```bash
ctest --test-dir build --output-on-failure   # unit tests, doctor, Arrival, SDK consumer, Python
./scripts/dogfood.sh                          # clean build, tests, sanitizers, live daemon smoke test
./scripts/lint.sh                             # clang-format, clang-tidy, cppcheck
./alignment.sh                                # project-wide C/C++ alignment sanitizer and tests
```

Version-tagged releases publish Linux SDK packages in `.deb` and relocatable
`.tar.gz` formats alongside a source archive. See [docs/RELEASING.md](docs/RELEASING.md)
for the tag, validation, and artifact details.

`scripts/dogfood.sh` builds from scratch, runs the suite under AddressSanitizer, UBSan and ThreadSanitizer, starts a real `gygax serve` process, drives it with `curl` and `gygax doctor --url`, then shuts it down with SIGTERM.

## Repository layout

```
src/gygax/        core, brain, hardware, signals modules (*.cppm) and plain C++ (net, inference, neuro, satlink, service, sim, transport, capi)
include/gygax/    public headers
tools/            gygax CLI, gyde (terminal IDE preview, docs/GYDE.md), 6502/6809 assembler and emulator
examples/         arrival (multi-agent + sim-to-real), wargames (autonomous defence, human-directable), ohflock (unarmed flock, 3D replay), satlink (pass planner, sample elements), hardware_demo, research_swarm, python, coco
python/           ctypes bindings and HTTP client (Python 3.9+, numpy optional)
deploy/           systemd unit, env file, compose file, Debian maintainer scripts
docs/             service, operations, neuromorphic, satlink, ohflock, architecture, transports, robotics, quantum integration, logistics, extensions, research, windows, gyde, atari, releases
```

## Platform support

Linux with clang 18+ is the reference platform and is what CI gates on. macOS builds with Homebrew's LLVM (`brew install llvm`; Apple's own clang has no C++26 yet), and CI builds it and runs the full test suite there as a best-effort job; the macOS job in [.github/workflows/ci.yml](.github/workflows/ci.yml) shows the exact flags. Windows is not supported natively; on Windows 11, `setup.ps1`/`build.ps1`/etc. run the same scripts through Docker Desktop (WSL2 backend), or use WSL2 directly. See [docs/WINDOWS.md](docs/WINDOWS.md).

## License

The Clear BSD License (BSD-3-Clause-Clear): see [LICENSE](LICENSE). It grants no patent rights; Neural Splines LLC's
patent position and what you may do without a separate licence are in [PATENTS.md](PATENTS.md). Third-party
components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
