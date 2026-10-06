# Research toolkit

Building blocks for robotics, reinforcement learning and neuromorphic research. Everything here is plain C++ with no external dependencies beyond the wrapped backends below, deterministic, and covered by loopback-free unit tests.

## Coordinate frames (`gygax/robotics/transform.hpp`)

`Quat`, `Transform` (SE(3)), `Mat4` conversion, `slerp`, and `TfBuffer`, a TF2-style tree.

- Each child frame has exactly one parent. `set` adds a timestamped sample, `setStatic` a fixed one. Cycles, self parents and re-parenting are rejected.
- `lookup(target, source, stamp)` returns the transform that maps points in `source` into `target`, interpolating linearly in translation and by slerp in rotation between the bracketing samples. Stamps outside the cached range fail unless they are within `maxExtrapolationSeconds`.
- `lookupLatest` uses the newest time common to every non-static edge on the path.
- The cache keeps `cacheSeconds` of history per frame.

## URDF and kinematics (`gygax/robotics/urdf.hpp`)

`Robot::parse` reads URDF with a small built-in XML parser (elements, attributes, comments, CDATA; no DTDs or entities beyond the five predefined ones). Supported joints: fixed, revolute, continuous, prismatic. Geometry, inertia and materials are ignored. Errors name the offending link or joint.

- `forwardKinematics(tip, q)` for any link.
- `jacobian` is a 6xN finite-difference Jacobian (linear rows first, then angular).
- `inverseKinematics` is damped least squares (via `math::linalg()`, see below) with joint-limit clamping and a step cap. It reports convergence, iteration count and residual errors, and never leaves joint limits.
- `publish` writes the joint state into a `TfBuffer` (fixed joints as static).

## Safety shield (`gygax/robotics/safety.hpp`)

- `VelocityShield` is a control barrier function filter for planar motion. Each circular obstacle and each geofence side contributes `dh/dt >= -alpha * h`; the desired velocity is projected onto the feasible set. `filterAlongHeading` gives the exact one-dimensional answer for differential-drive robots that cannot move sideways. `infeasible` is set when no velocity satisfies every constraint; the caller must stop.
- `JointShield` enforces position, velocity and acceleration limits and brakes ahead of position limits. Non-finite commands hold position.

The shield only knows the obstacles it is given. It is a filter in front of the actuators, not a substitute for a hardware emergency stop.

## Fast/slow control (`ActionChunkEnsembler`, `TargetHold`)

`ActionChunkEnsembler` implements ACT-style temporal ensembling: overlapping action chunks predicted by a slow model are blended per step with weights `exp(-decay * rank)`, oldest first. `TargetHold` keeps the fast loop running when the slow planner stalls: after `timeoutSeconds` without a new target it returns the fallback.

## Vectorized simulation (`gygax/sim/vec_env.hpp`)

`VecEnv` steps N independent rover navigation worlds (`World2D`) into flat float buffers, ready to wrap as NumPy arrays.

- Observation (`beams + 4`): normalized lidar, goal distance, goal bearing, linear and angular velocity. Action: two floats in [-1, 1] (forward, turn).
- Reward: progress toward the goal, a step cost, a collision penalty and a goal bonus. `terminated` means the goal was reached, `truncated` means `maxSteps`.
- With `autoReset`, finished environments are reset in place and their last observation is kept in `finalObservations()`.
- Domain randomization draws per episode from ranges: motor gain, wheel base, lidar noise and dropout, pose noise, actuation delay in steps, and dropped actions.
- Every environment owns its RNG derived from the seed and its index, so results are identical for any `threads` value.

## Event cameras and spiking decoders (`gygax/neuro/events.hpp`)

- `decodeEvt2` parses the Prophesee EVT 2.0 raw format (CD on/off and time-high words; other words are counted and skipped). AEDAT4, `.dat` and vendor drivers are not implemented.
- `eventFrame`, `timeSurface`, `voxelGrid` build tensors from events; `eventsToSpikes` and `EventInjector` feed them into a spike source population.
- `decodePopulationVector`, `RateDecoder` (exponential smoothing) and `PdmModulator` (first-order delta-sigma) turn motor spikes into commands.

## Backends behind interfaces

External libraries are never used directly by callers. Each is wrapped by an abstract backend so another implementation can be swapped in at run time.

| Front end | Interface | Default backend | Swap with |
| --- | --- | --- | --- |
| Dense linear algebra (`math::Matrix`, `linalg()`) | `math::LinearBackend`: `solve`, `multiply`, `dampedLeastSquares` | Eigen (`makeEigenBackend`) | `setLinearBackend` |
| Arbitrary-size integers (`math::BigInt`) | `math::IntegerBackend` / `IntegerRep` | GMP (`makeGmpBackend`) | `setIntegerBackend` |

Eigen and GMP headers appear only in `src/gygax/math/*.cpp`. `BigInt` values are immutable and shared, and mixing values from two backends converts through decimal strings. URDF inverse kinematics runs through `linalg()`. Install `libeigen3-dev` and `libgmp-dev` (Homebrew: `eigen gmp`) to build. A test swaps in a counting backend to prove callers do not depend on Eigen.

## Python and Gymnasium (`gygax.rl`)

`VecEnv` in C++ is exposed through a small C ABI (`gygax_vecenv_*` in `gygax/capi/gygax.h`) that takes a JSON config (keys mirror `VecEnvConfig`, ranges as `[lo, hi]`, validated in `vecEnvConfigFromJson`). `gygax.rl.VecEnv` wraps it with NumPy and returns `(obs, reward, terminated, truncated, info)` batches. `gygax.rl.GygaxRoverEnv` is a single-environment class that follows the Gymnasium `Env` contract; it derives from `gymnasium.Env` and defines spaces when `gymnasium` is installed and works as a plain object otherwise. Stable-Baselines3 and CleanRL need only this class; the batched `VecEnv` is meant for custom training loops.

```python
from gygax import rl
env = rl.VecEnv(num_envs=1024, threads=8, seed=1, randomization={"motor_gain": [0.8, 1.2]})
obs, _ = env.reset(1)
obs, reward, terminated, truncated, info = env.step(actions)
```

## Language models and MCP

- **Engines.** `--engine ollama`, `--engine llama-cpp` and `--engine lmstudio` (also `NAME@host:port`, or a full `http://host:port/v1`) all speak the standard chat-completions API (`/v1/chat/completions`) that those servers provide. Defaults: 11434, 8080 and 1234. Any engine works for chat, streaming, health probing and failover through `EnginePool`, and for the agent tool loop.
- **MCP client.** `--mcp NAME=COMMAND ARGS` (or `GYGAX_MCP`) starts a Model Context Protocol server over stdio, negotiates `initialize`, lists its tools (with pagination) and registers each as `mcp.NAME.TOOL` for agents and `/v1/tools/.../invoke`. Tool names are sanitized to `[A-Za-z0-9_-]`. Errors reported by the server (`isError`) become tool failures. Only stdio transport and text content are supported; other content is replaced by a placeholder.
- **MCP server.** `serveMcp` in `gygax/service/mcp.hpp` serves any tool set over stdio so an MCP host such as LM Studio can use it. `wargames --mcp-serve` exposes the `war.*` tools this way.

## Wargames proof

`wargames --selfcheck` (a ctest entry) shows that a battle decided over HTTP is identical to the in-process one. A loopback server implements the standard chat-completions API by running the doctrine; the same battle is fought through the `ollama`, `llama-cpp` and `lmstudio` engine presets (about 7,000 model calls each) and must reproduce the same digest and tool-call count. It then starts `wargames --mcp-serve` as a child process, lists the tools with the MCP client and checks `war.situation` against the in-process battlefield. No real model server is used in the tests: to try one, run `wargames --engine ollama --model llama3.1` (a language model will not reproduce the doctrine's results).

## Seek & Find visual simulation

[`examples/seek_and_find/seek_and_find.html`](../examples/seek_and_find/seek_and_find.html)
is an interactive, standalone fictional replay of simulated scouts restoring
a communications path, illustrating basis sifting, using a mock
quantum-inertial heading estimate for a waypoint, locating a UV-visible prop,
and guiding an inspection drone along visual breadcrumbs. The companion
[USD scene](../examples/seek_and_find/seek_and_find.usda) is provided for
inspection in a USD-compatible viewer. These assets do not connect to Gygax,
quantum sensors, QKD equipment, radios, GPS, real robots, or external services;
they generate no key material and are illustrative demo content, not evidence
of hardware readiness or a real-world tracking method. Vendor-neutral
integration interfaces and limitations are described in
[Quantum integration](QUANTUM_INTEGRATION.md).
Games and races can be explored in simulation, but applying a learned or
simulated policy to real machines requires separate authorization, independent
safety review, and controlled validation.

## Not implemented

Not started here, since each needs external SDKs, GPUs or hardware, or a substantial new subsystem: native ROS 2/DDS nodes and action clients, MoveIt/Nav2, MCAP, 3D physics and MuJoCo/Isaac backends, ONNX/LibTorch policy execution, VLA clients and multimodal chat, Rerun/Foxglove/W&B, LeRobot/HDF5 export, DLPack, differentiable SNNs and silicon exporters, and the ALIF/COBA/R-STDP neuron and synapse models.
