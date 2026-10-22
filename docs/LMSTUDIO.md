# Using gygax from LM Studio (MCP)

`gygax mcp` runs gygax as an MCP server over stdin/stdout. LM Studio (0.3.17 or later)
starts it for you and lets any loaded model call gygax's tools.

## Setup

1. Build gygax (`./build.sh`) and note the full path to the binary, e.g.
   `/home/you/ns-gygax/release/build/gygax`.
2. In LM Studio open **Program → Install → Edit mcp.json** (the file lives at
   `~/.lmstudio/mcp.json`) and add:

```json
{
  "mcpServers": {
    "gygax": {
      "command": "/home/you/ns-gygax/release/build/gygax",
      "args": ["mcp"]
    }
  }
}
```

3. Save. LM Studio starts gygax and lists its tools (`math__eval`, `time__now`,
   `device__list`, `logistics__plan`, ...). Pick a model with tool-use support and chat.

## Notes

- Tool names: MCP clients require `[A-Za-z0-9_-]`, so gygax's dotted names travel with
  `__` in place of `.` (`math.eval` → `math__eval`).
- Arguments: every tool takes one optional `input` string (plain text or JSON text, as the
  tool's description says). If a model passes fields directly instead, gygax forwards them
  as JSON.
- Parallel calls: tool calls run on a worker pool, one thread per spare core, each pinned
  to a core next to the one reading requests, so several requests at once don't queue
  behind a slow tool. Set the pool size with `--mcp-workers N`.
- All `serve` options work here too. For example, to expose robots and other MCP servers:
  `"args": ["mcp", "--device-commands", "--device", "uav=mavlink:udp://0.0.0.0:14550"]`.
- Logs go to stderr (LM Studio shows them in its developer log); stdout carries only the
  protocol. Raise detail with `--log-level debug`.

## Check it without LM Studio

```sh
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"math__eval","arguments":{"input":"2^10"}}}' \
  | ./build/gygax mcp
```
