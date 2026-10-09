# Gyde: the Gygax IDE

Gyde ("guide") is the start of a dedicated environment for Gygax: edit, type commands, debug, monitor, inspect, train, and broadcast, without needing a general-purpose IDE. This document says plainly what exists today. Nothing beyond what is described here should be assumed to work.

## Why not just an IDE plugin

You never need an IDE to develop Gygax today: `./build.sh`, `./scripts/lint.sh`, `gdb`/`lldb`, and `curl` against a running service are the whole workflow, and that stays true regardless of what Gyde becomes; Gyde is additive, not a requirement. The case for a dedicated tool instead of, say, a general-purpose editor extension is that Gygax is not only source code: a running instance has live engines, agents, a neuromorphic network with membrane traces and spike rasters, a device registry with real hardware links, and a logistics ledger, which is state an editor has no model for. Gyde's job is to be a control surface for that runtime state as much as for the source.

## The console

`tools/gyde.cpp` (built as `gyde`, `gyde_selfcheck` in ctest) is a small, real terminal client:

- Every prompt line is a status flash (`[up] engines 1/1  agents 0  http://127.0.0.1:1984`), refreshed after every command. Whatever you were just looking at, the next thing you see is current status, not stale output.
- Typed commands: `status` (node/engines/agents in full), `engines` (a compact health table), `models`, `ask <text>` (a chat completion), `tool <name> <input>` (invoke any registered tool, built-in, plugin, extension, or MCP), `help`.
- It is an ordinary HTTP client against the existing `/v1/*` API (`include/gygax/net/http.hpp`), so it needs no new server-side surface and works against any running Gygax node, local or remote.

```
$ gyde --url http://127.0.0.1:1984 --token "$GYGAX_API_TOKEN"
gyde 0.3.0 - type 'help' for commands, 'quit' to leave
[up] engines 1/1  agents 0  http://127.0.0.1:1984 > ask what is sqrt(2)*sqrt(2)?
echo: what is sqrt(2)*sqrt(2)?
[up] engines 1/1  agents 0  http://127.0.0.1:1984 > tool math.eval 6*7
{"tool":"math.eval","output":"42"}
```

## What this is not (yet)

No panels, no logo, no drag-and-drop layout, no debugger, no training UI, no broadcast, no Python extension loading. Everything above is optional tooling on top of a runtime that already works fully from the command line and HTTP. Gyde's success condition is that it makes the runtime's live state easier to see and act on, not that it becomes required.
