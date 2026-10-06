# Extensions and plugins

Two ways to add tools that agents and API clients can call, plus the Python and C SDKs that go with them.

The installed native API versions, ownership rules, and hardware trust limits
are documented in [Public API and trust boundaries](SDK_API.md).

| | Native plugin | Extension process |
| --- | --- | --- |
| Language | C, C++ or anything that exports a C symbol | Python (SDK included), or anything that speaks HTTP |
| Runs | inside the service process | in its own process |
| Failure isolation | none: a crash takes the service down | full |
| Latency | function call | one HTTP round trip |
| Tool names | `plugin.<plugin>.<tool>` | `ext.<extension>.<tool>` |
| Enable | `--plugin PATH` / `GYGAX_PLUGINS` | `--extension URL` / `GYGAX_EXTENSIONS` |

Prefer extension processes unless you need in-process speed. Both are configured only at startup; there is no API to load code at runtime.

## Python extension SDK

```python
from gygax import Extension

ext = Extension("ops", "1.0.0", token="s3cret")

@ext.tool(description="Upper-case the input")
def shout(text):
    return text.upper()

@ext.tool(json_input=True)
def pallets_needed(args):
    return {"pallets": -(-args["items"] // args["per_pallet"])}

ext.serve(port=9000, block=True)
```

```bash
gygax serve --extension "http://127.0.0.1:9000;token=s3cret"
curl -X POST localhost:1984/v1/tools/ext.ops.shout/invoke -d '{"input":"hi"}'
```

Tool functions receive the input string (or the parsed JSON with `json_input=True`) and return a string or any JSON-serializable value. Exceptions become tool errors the agent can read. Non-loopback listeners require a token.

### Wire protocol

Any language can implement it:

- `GET /gygax/describe` returns `{"name":"ops","version":"1.0.0","tools":[{"name":"shout","description":"..."}]}`. Names: extension `[a-z0-9_-]{1,32}`, tool `[A-Za-z0-9_-]{1,48}`, at most 128 tools.
- `POST /gygax/call/{tool}` with `{"input":"..."}` returns `200 {"output":"..."}` or a 4xx/5xx `{"error":"..."}`.
- With `;token=T` on the URL the service sends `Authorization: Bearer T` on both.

The service fetches the description once at startup and fails to start if the extension is unreachable. Restart the service to pick up new tools.

## Native plugin SDK

C ABI, version 1, header `gygax/sdk/plugin.h`. A plugin exports `gygax_plugin_entry` returning a `gygax_plugin` descriptor: name, version, tool list, a `call(tool, input, &output)` function returning 0 on success (on failure the output is the error message), a `release(buffer)` function that frees what `call` allocated, and `shutdown`.

C++ users can use `gygax/sdk/plugin.hpp`:

```cpp
#include <gygax/sdk/plugin.hpp>

static gygax::sdk::Plugin& plugin() {
    static gygax::sdk::Plugin p = [] {
        gygax::sdk::Plugin built("fleet", "0.1.0");
        built.tool("echo", "Return the input", [](const std::string& in) { return in; });
        return built;
    }();
    return p;
}
GYGAX_PLUGIN(plugin())
```

Build it against an installed Gygax:

```cmake
find_package(Gygax 0.2 REQUIRED)
gygax_add_plugin(fleet_plugin SOURCES fleet_plugin.cpp)
```

Complete, tested examples: `examples/plugin/geo_plugin.cpp` (C++), `examples/plugin/c_plugin.c` (plain C) and `examples/plugin-consumer/` (an out-of-tree project; `scripts/sdk-check.sh` installs Gygax, builds it against the package and calls it through a running service, and runs as the `sdk_consumer` test).

Rules for plugin code: tools may be called from several threads at once, so be thread-safe; exceptions must not escape `call`; output is limited to 4 MiB; the service cannot interrupt a tool that hangs.

### Trust

Loading a plugin runs its code with the service's privileges. The loader refuses files that are not regular files, not owned by root or the service user, writable by others, or writable by a group other than the owner's own. That stops accidental loads of tampered files; it is not a sandbox. Only load plugins you built or reviewed, install them in a root-owned directory, and prefer extension processes for anything third party.

The C plugin ABI is versioned independently from the project release and
requires an exact ABI version match. The service may call a plugin tool
concurrently. Neither the ABI nor the C++ helper supplies a human approval
step or makes a plugin safe to run.

## Python package

`python/` builds into the tree as `build/python` on every `cmake --build` (target `gygax_python`), with the native library next to it, so `PYTHONPATH=build/python` is all a researcher needs:

```python
from gygax import LocalService, Extension, neuro

with LocalService() as svc:
    print(svc.client.logistics.summary("7d"))
```

`LocalService` starts `gygax serve` on a free loopback port (`GYGAX_BIN` or `PATH`), waits for it to be healthy and stops it on exit. `cmake --install` puts the package under `share/gygax/python` and the SDK headers, CMake package and examples under the prefix.
