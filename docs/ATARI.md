# ATARI: a compact agent tool-calling protocol

Gygax's default agent tool-calling protocol asks a model to reply with a JSON object: `{"tool": "<name>", "input": "<string>"}` to call a tool, or `{"answer": "<text>"}` when done, and the tool's result is fed back as free text (`Tool result (<name>): <output>`). ATARI ("Agent Tokenized/Terse Agent Relay Interface") is an additive, opt-in alternative wire format for the exact same loop, designed to spend fewer tokens per turn. Nothing about tools, the agent state machine, or the JSON protocol's continued existence changes.

## Enabling it

- Service/CLI: `gygax serve --tool-protocol atari` (or `GYGAX_TOOL_PROTOCOL=atari`). Default is `json`; an unrecognized value fails `ServiceConfig::validate`.
- C++: `OrchestratorOptions::protocol = ToolProtocol::Atari` (`gygax/brain/orchestrator.cppm`, a module: `import gygax.orchestration;`).
- It works with any backend, including the built-in `echo` engine (`EchoBackend` speaks both protocols; see below), so `gygax doctor`/CI can exercise it with no real model.

## Grammar

A frame is `ATARI::TYPE(key=value;key=value)`:

| Frame | Meaning | Sent by |
| --- | --- | --- |
| `ATARI::CALL(t=<code>;i=<input>)` | Call a tool | the model |
| `ATARI::DONE(a=<answer>)` | Final answer | the model |
| `ATARI::RESULT(t=<code>;o=<output>)` | A tool call succeeded | the orchestrator |
| `ATARI::ERROR(t=<code>;e=<message>)` | A tool call failed | the orchestrator |
| `ATARI::SCHEMA(<code>=<name>;...)` | Declares tool-name codes | the orchestrator, once, in the system prompt |

Keys (`t`, `i`, `o`, `a`, `e`) are fixed by the grammar itself and are never declared. Only tool *names*, which repeat every step of a multi-step run, are interned. Values may contain any bytes; a literal `;`, `)` or `\` inside one must be backslash-escaped (`\;`, `\)`, `\\`); `=` needs no escaping. `gygax::inference::atari::parse` finds the first well-formed frame anywhere in a string and returns `FrameType::Unknown` if none is found, so a plain-text answer with no frame is handled the same way the JSON path treats non-JSON text: as the answer.

## Schema: the "tree cache"

`atari::Schema` assigns each tool name a short sequential code (`c0`, `c1`, ...) the first time it is interned, and resolves codes back to names through a prefix tree (trie) in O(code length), not a scan. `Orchestrator::systemPrompt` interns every currently-registered tool up front, so the very first `ATARI::SCHEMA(...)` frame (in the system prompt) already covers the whole run, and every `CALL`/`RESULT`/`ERROR` for the rest of that run references a 2-3 character code instead of a dotted tool name. The schema lives for the duration of one agent run (it is a local variable in `Orchestrator::runJob`); it is not currently shared or persisted across separate runs or agents. Extending it to a longer-lived, cross-run cache is future work, not implemented here.

A peer that receives frames out of band (not by reading the system prompt) can learn the same mapping with `Schema::mergeSchemaFrame`, which rejects a `SCHEMA` frame that tries to redefine an existing code to a different name.

## Example

```
$ gygax serve --engine echo --tool-protocol atari --token t &
$ curl -s localhost:1984/v1/chat/completions -H "Authorization: Bearer t" \
    -d '{"model":"gygax-agent:echo","messages":[{"role":"user","content":"!tool math.eval 2^10"}]}'
{"choices":[{"message":{"content":"echo: 1024", ...}}], ...}
```

Wire trace for that run (system prompt shortened; codes are assigned in alphabetical order over every registered tool, so `math.eval` is `c6`, after `device.list`, `device.state` and the four `logistics.*` tools):

```
system: You are a Gygax autonomous agent. ... Tool codes:
        ATARI::SCHEMA(c0=device.list;c1=device.state;c2=logistics.plan;c3=logistics.record;
                      c4=logistics.summary;c5=logistics.units;c6=math.eval;c7=neuro.run;
                      c8=node.info;c9=time.now;c10=webpage.construct)
        Tools:
        - c6: Evaluate an arithmetic expression: ...
user:   !tool math.eval 2^10
assistant: ATARI::CALL(t=c6;i=2^10)
user:   ATARI::RESULT(t=c6;o=1024)
assistant: ATARI::DONE(a=echo: 1024)
```

## Measured savings

`tests/unit/test_atari.cpp` (`AtariProtocol.IsShorterThanTheEquivalentJsonToolCall`) checks the concrete byte counts, not just an estimate:

| Message | JSON | ATARI (interned) | 
| --- | --- | --- |
| Call `math.eval` with `6*7` | `{"tool": "math.eval", "input": "6*7"}` (38 bytes) | `ATARI::CALL(t=c0;i=6*7)` (24 bytes) |
| Tool result | `Tool result (math.eval): 42` (28 bytes) | `ATARI::RESULT(t=c0;o=42)` (25 bytes) |

The saving is modest on a single call and compounds over a multi-step run: every repeat reference to an already-interned tool, in either direction, costs a 2-3 character code instead of a full dotted name. The very first turn's tool *descriptions* (what each tool does) are not compressed, because that content is necessary regardless of wire format. Byte count is used here instead of a real BPE token count because it is a fair, reproducible proxy; actual token savings depend on the model's tokenizer but track byte count closely for text this short and punctuation-heavy.

## What this is not

Not a transport and not a replacement for MCP (`docs/RESEARCH.md`): those solve different problems (MCP is a tool-hosting protocol between processes; ATARI is a wire format between the orchestrator and one model within a single run). Not a compression scheme beyond the schema interning described above: no arithmetic/entropy coding, no binary encoding, no "screens as language" pixel/raster encoding. That idea (representing a frame as a tiny bitmap, closer to how the Atari 2600's TIA drew a screen from a handful of registers) is a real possibility for a future, denser codec but is not implemented; ATARI v1 is plain, escaped text.
