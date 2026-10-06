# Gygax public API and trust boundaries

```text
   _______  __   __  _______  _______  _______
  |       ||  | |  ||       ||       ||       |
  |   _   ||  |_|  ||    ___||    ___||  _____|
  |  |_|  ||       ||   | __ |   |___ | |_____
  |______||_______||___||__||_______||_______|
            TRU5T = EXPLICIT CONTRACTS
```

This page describes the installed native SDK surface and what it does not
promise. The intent is boring in the best way: small interfaces, visible
ownership, explicit version checks, and no implied authority.

## What ships as the native SDK

The install provides the CMake target `gygax::sdk`, C and C++ plugin headers,
the C simulation API header, the generated C++ version header, and plugin
examples. The C API currently covers neuromorphic networks and vectorized
simulation environments; it is not a hardware-control API.

| Version identifier | Meaning | Compatibility behavior |
| --- | --- | --- |
| Project version (`0.2.0` today) | Product release; available as `GYGAX_VERSION_*`, `GYGAX_VERSION_STRING`, and `gygax_version()` | Pre-1.0 releases do not promise source or binary compatibility between releases. Rebuild consumers and read release notes when upgrading. |
| `GYGAX_PLUGIN_ABI_VERSION` (`1`) | Layout and callbacks in `gygax/sdk/plugin.h` | The loader requires an exact match and rejects other values. There is no negotiation or compatibility shim. |
| CMake package version | `find_package(Gygax <version>)` package selection | CMake's `SameMajorVersion` package check is a package-selection rule, not proof that arbitrary headers or binaries are ABI-compatible. |

The C++ plugin helper is source code built into the plugin; it does not create
a separate stable C++ binary ABI. Keep plugin and host releases aligned, and
rebuild plugins against the SDK release being deployed.

## Interface boundaries

- **C simulation API:** opaque handles manage simulation state. Gygax owns its
  returned strings (release with `gygax_free`) and environment result arrays
  (borrowed until the next reset, step, or destruction). Check the function's
  return contract and `gygax_last_error()` on the calling thread.
- **Plugin ABI:** a narrow C descriptor exposes named tools and explicit buffer
  release. The host may call tools concurrently. In-process plugins share the
  host's privileges and failure domain; the ABI is not a sandbox.
- **Extension process:** use a separate process when failure isolation or a
  stronger trust boundary matters. Process separation is not itself a complete
  security boundary; deployment permissions and authentication still matter.
- **Transport, device, and simulation interfaces:** transports move protocol
  data, devices expose structured state and commands, and simulation models
  represent a configured virtual system. Selecting one does not silently
  authorize or substitute for another.

## Human authority and physical systems

Device commands are disabled by default. Starting the service with
`--device-commands` or `GYGAX_DEVICE_COMMANDS=1` enables command tools; it does
**not** add an operator confirmation step. When enabled, a request may reach a
device without a human reviewing that individual command. Gygax does not
provide a built-in human-in-the-loop approval gate, certified safety function,
or universal hardware/simulation interlock.

If a deployment requires human approval, implement it as an explicit,
authenticated gate outside the agent's authority path, make its state visible
to the operator, and test that commands cannot bypass it. Keep machine-side
limits, watchdogs, failsafes, and an independent emergency stop. A simulated
run is useful for rehearsal and software testing; it does not establish that a
real machine, environment, operator procedure, or command is safe.

Tests establish behavior of the tested software under their stated conditions.
They are not certification, proof of physical safety, proof of security, or a
substitute for validation against the actual device and deployment.

## Upgrade checklist

1. Compare the project version and plugin ABI version with the deployed host.
2. Read the release notes and rebuild native consumers for the target release.
3. Review plugin code and its permissions; prefer an extension process for
   untrusted code.
4. Confirm whether the deployment is connected to a simulator or physical
   device, and verify command authorization and emergency-stop behavior
   independently before enabling commands.
