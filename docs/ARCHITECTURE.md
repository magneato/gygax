# Architecture

## Layers

```
                       gygax CLI (serve, doctor, rpc, neuro, status, ask)
                                        |
     HTTP server ---- service (routes, auth, rate limit, metrics, JSON-RPC)
        |                 |                       |
     net/http       inference pool          agent runtime (orchestrator)
                    (engines, routing,       |        |          |
                     failover, probing)   tools   world model   event relay
                                                      |
   hardware nodes ── state machines ── message hub ── embodiments (taxonomy)
        |                                                  |
   neuro (LIF, STDP) ---- C ABI ---- Python        sim (2D world, LiDAR, USD, robot link)
                                                           |
                                                   transports (serial, GPIO, modem, printer, G-code, UDP/TCP)
```

   For a standalone visual example of simulated message-path recovery and a
   non-operational visual search, see [Seek & Find](../examples/seek_and_find/README.md).
   It is not a live service, communications test, or hardware-control demo.

## Source map

C++26 module interfaces (`*.cppm`) hold the stateful runtime; plain headers and `.cpp` files hold everything that benefits from fast builds and lint coverage.

| Path | Module or namespace | Role |
| --- | --- | --- |
| `src/gygax/core` | `gygax.core.base`, `gygax.core.messaging` | `BaseObject` lifecycle, parameters, thoughts; `Hub` message router with temporal `Hashbelt` buffering |
| `src/gygax/environment` | `gygax.world_model` | Thread-safe per-agent state (`LatentSpace`, `EpisodicMemory`, `StateMachineState`), swarms |
| `src/gygax/signals` | `gygax.signals`, `gygax.messaging` | Publish/subscribe event relay with bounded per-subscriber queues |
| `src/gygax/skills` | `gygax.tools` | Tool registry |
| `src/gygax/bus`, `src/gygax/robotics` | `gygax::bus`, `gygax::robotics` | CAN, ISO-TP, OBD-II, J1939, DBC, CANopen, Modbus, ARINC 429; MAVLink, NMEA, ADS-B, rosbridge, `Device` registry, virtual autopilot ([ROBOTICS.md](ROBOTICS.md)) |
| `src/gygax/logistics` | `gygax::logistics` | Ledger, GUIDs, journal, route planner ([LOGISTICS.md](LOGISTICS.md)) |
| `src/gygax/service/plugins.cpp` | `gygax::service` | Native plugin loader and extension-process client ([EXTENSIONS.md](EXTENSIONS.md)) |
| `src/gygax/brain` | `gygax.orchestration`, `gygax.state_machine`, `gygax.taxonomy.base`, ... | Agent runtime, hierarchical state machines, embodiments |
| `src/gygax/hardware` | `gygax.hardware.*` | `Node` trees, sensors, actuators, links, compute nodes including `NeuromorphicNode` |
| `src/gygax/inference` | `gygax::inference` | `Backend`, `EchoBackend`, `ChatApiBackend`, `EnginePool` |
| `src/gygax/net` | `gygax::net`, `gygax::comm` | HTTP/1.1 server and client, UDP and TCP streams |
| `src/gygax/service` | `gygax::service` | The service, expression evaluator, node info |
| `src/gygax/neuro` | `gygax::neuro` | Spiking network engine |
| `src/gygax/sim` | `gygax::sim` | 2D world and robot link |
| `include/gygax/quantum` | `gygax::quantum` | Vendor-neutral navigation-estimate and QKD secure-channel adapter interfaces; no hardware providers included ([Quantum integration](QUANTUM_INTEGRATION.md)) |
| `src/gygax/transport` | `gygax::transport` | Device transports |
| `src/gygax/capi` | C ABI | `libgygax_c` |

## Threading model

- `net::Server`: one acceptor thread, a fixed worker pool, a bounded connection queue. Handlers are ordinary functions; streaming handlers write through a `ChunkWriter` whose status and headers stay changeable until the first chunk.
- `Orchestrator`: a fixed worker pool executes objective loops; per-agent state lives in the `WorldModel` behind a shared mutex with snapshot/mutate access, never raw pointers. Cancellation is checked between steps.
- `EnginePool`: state under one mutex, upstream calls outside it; an optional prober thread refreshes inventories.
- `GlobalEventRelay`: every subscriber owns a bounded queue (drop-oldest, counted), so slow observers cannot stall producers or steal events from each other.
- `Hub`: a recursive mutex allows handlers to reply to messages; offline targets get their messages on registration.
- Embodiments run a condition-variable-driven worker (20 ms period), not a spin loop; `advance(dt)` steps them manually and deterministically.
- The test suite runs clean under AddressSanitizer, UBSan and ThreadSanitizer (`scripts/dogfood.sh`).

## Agent loop

1. `POST .../objectives` marks the agent `running`, publishes `UserObjective`, and queues the run.
2. A worker builds messages: system prompt (protocol marker, tool list, last five memory entries) and the objective.
3. Each step calls the backend (`PoolBackend` over the `EnginePool`), records the prediction in episodic memory and publishes `ModelPrediction`.
4. A reply containing `{"tool": ...}` triggers `ActionCall`, the tool runs (exceptions become error text), and the result is appended as a user message. `{"answer": ...}` or plain text completes the run.
5. Hitting `max_steps`, an engine error or cancellation marks the run `failed` with a reason. Cancellation is cooperative: it cannot interrupt a backend request or tool already in progress. If cancellation arrives during a backend request, the returned action is discarded before tool dispatch.

## Extension points

- **Tools**: `tools::ToolRegistry::getInstance().registerTool(name, fn, description)`.
- **Inference backends**: implement `inference::Backend` (`chat`, optional `chatStream`, `listModels`) and add it to an `EnginePool`, or expose it over HTTP and configure it as an engine.
- **Embodiments**: implement a `taxonomy::Brain` subclass and register it with `FACTORY_REGISTER`.
- **Transports**: implement `transport::Transport` or `GpioPort`.
- **Robot links**: implement `sim::RobotLink`; `StreamRobotLink` sends JSON lines over any `comm::Stream`.

## Deliberate limits

- Agent and engine state is in memory; there is no persistence layer.
- The built-in HTTP client and server are plain HTTP. TLS termination belongs in front of the service.
- Peer discovery is configured, not automatic.
- Removed code, and why, is listed in `trash/README.md`.
