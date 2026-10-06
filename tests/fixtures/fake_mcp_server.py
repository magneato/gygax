import json
import sys

JSON_RPC_VERSION = "2.0"
MCP_PROTOCOL_VERSION = "2024-11-05"

TOOLS = [
    {"name": "add", "description": "Add two numbers", "inputSchema": {"type": "object"}},
    {"name": "fail.now", "description": "Always fails", "inputSchema": {"type": "object"}},
    {"name": "echo", "description": "Echo input", "inputSchema": {"type": "object"}},
]


def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    msg = json.loads(line)
    method = msg.get("method")
    if "id" not in msg:
        continue
    rid = msg["id"]
    if method == "initialize":
        send({"jsonrpc": JSON_RPC_VERSION, "method": "notifications/message", "params": {"level": "info"}})
        send({"jsonrpc": JSON_RPC_VERSION, "id": rid, "result": {"protocolVersion": MCP_PROTOCOL_VERSION, "capabilities": {"tools": {}},
                                                       "serverInfo": {"name": "fake", "version": "9.9"}}})
    elif method == "tools/list":
        send({"jsonrpc": JSON_RPC_VERSION, "id": rid, "result": {"tools": TOOLS}})
    elif method == "tools/call":
        params = msg["params"]
        args = params.get("arguments", {})
        if params["name"] == "add":
            text = str(args.get("a", 0) + args.get("b", 0))
            send({"jsonrpc": JSON_RPC_VERSION, "id": rid, "result": {"content": [{"type": "text", "text": text}]}})
        elif params["name"] == "echo":
            send({"jsonrpc": JSON_RPC_VERSION, "id": rid, "result": {"content": [{"type": "text", "text": json.dumps(args)}]}})
        elif params["name"] == "fail.now":
            send({"jsonrpc": JSON_RPC_VERSION, "id": rid, "result": {"isError": True, "content": [{"type": "text", "text": "boom"}]}})
        else:
            send({"jsonrpc": JSON_RPC_VERSION, "id": rid, "error": {"code": -32601, "message": "unknown tool"}})
    else:
        send({"jsonrpc": JSON_RPC_VERSION, "id": rid, "error": {"code": -32601, "message": "unknown method"}})
