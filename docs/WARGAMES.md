# Wargames: the last stand

For a non-combat communications-and-search simulation, see the standalone
[Seek & Find demo](../examples/seek_and_find/README.md), with an
[interactive HTML replay](../examples/seek_and_find/seek_and_find.html) and
[USD scene](../examples/seek_and_find/seek_and_find.usda). It is fictional
simulation content and does not control hardware or communicate with radios.
The adjacent demo includes a classical basis-sifting illustration and a mock
navigation-sensor estimate; neither represents deployed quantum technology.
See [Quantum integration](QUANTUM_INTEGRATION.md) for the adapter boundaries.

`build/wargames` is a showcase in which Gygax agents command every defender of a last bunker against six waves of an alien invasion. A human can direct the agents while it runs.

```bash
./build/wargames --map                       # watch the battle, ASCII map as each wave lands
./build/wargames --html build/wargames.html  # generate a graphical replay of the real simulation
./build/wargames --interactive               # start paused and direct the agents yourself
./build/wargames --script examples/wargames/orders.txt
./build/wargames --difficulty 3              # the bunker falls
./build/wargames --selfcheck                 # deterministic verification, also a ctest entry
```

## What is real

- **Agents.** The bunker, eight turrets, tanks, interceptor drones and supply drones each have a Gygax `Orchestrator` agent. Every simulated second each agent runs the normal tool loop: observe, report, plan, act, answer. The tools are registered in the `ToolRegistry` (`war.observe`, `war.plan`, `war.report`, `war.act`, `war.consensus`, `war.rally`, `war.produce`, `war.situation`). A full battle is about 18,000 model calls and 13,000 tool calls.
- **The model is a doctrine, not an LLM by default.** `--engine ollama|llama-cpp|lmstudio|URL` swaps in a language model, and `--mcp-serve` exposes the `war.*` tools to MCP hosts. `--selfcheck` proves the wiring by fighting the same battle through a loopback chat-completions server and comparing digests.
The default `Backend` is a deterministic rule set (`DoctrineBackend`), so battles are reproducible and need no network. Agents only see the tool protocol, so the backend is the one piece a language model replaces. A real model has not been run against it here; the selfcheck uses a loopback server that answers with the doctrine.
- **Collective intelligence.** Combat units report the sector they believe is most threatened into `CollectiveBrain`; the commander agent asks for the consensus (noisy-OR over the reports) and rallies the mobile units to it.
- **Supply chain.** Every unit, human or alien, has a GUID in a `logistics::Ledger`. Deployments, factory output, invasion waves, kills and losses are ledger events, and the final report is the ledger's view. Interceptors and supply drones use `planRoute` with the base and four refuel pads to check a sortie is within range and to follow refuel stops. Supply drones carry ammunition from the bunker to turrets and tanks that are running dry.
- **Spiking hive.** The aliens' rage is a small LIF network fed by casualties (`neuro::Network`); when it fires, swarmers speed up.
- **Deterministic.** Same seed and same directives give the same battle (checked by digest in `--selfcheck`). Agents are run one at a time in id order.

The physics is a deliberately simple 2D model (constant speeds, damage per second, ammunition, fuel). It is a showcase, not a combat simulator.

## Directing the agents

Directives change what the agents are told and how they decide. They are available on stdin with `--interactive` and from a timed script with `--script`. Both use the same grammar; invalid ones are rejected with an explanation.

| Directive | Effect |
| --- | --- |
| `rally N\|NE\|E\|SE\|S\|SW\|W\|NW` / `rally auto` | Mobile units rally to a sector; `auto` returns the choice to the agents' consensus |
| `priority threat\|spitter\|bruiser\|swarmer\|nearest` | How every agent picks targets |
| `weapons free\|tight\|hold` | Tight conserves ammunition on weak targets; hold ceases fire |
| `drones hunt\|recall` | Interceptors hunt or return to the bunker |
| `focus <alien id>` / `focus off` | Every weapon in range focuses one target |
| `produce interceptor\|tank\|supply-drone` | The commander agent builds it if materiel allows |
| `autoproduce on\|off` | Whether the commander replaces losses on its own |
| `note <text>` | Free text added to every agent's orders (meaningful once a language model is behind the agents) |

Console commands in interactive mode: `status`, `map`, `pause`, `resume`, `step [n]`, `pace <seconds>`, `help`, `quit`. Script files contain `<seconds> <directive>` lines; `#` starts a comment line.

The simulation is paused until you type `resume` in interactive mode; while paused you can issue directives and `step` one second at a time. Alien ids appear in the narrative (`bruiser A54 down`) and in the agents' observations.

## Outputs

`--json FILE` writes the outcome, statistics, the survivors and the full ledger summary. The final report prints the ledger flow: units in the field per SKU with active, lost and retired counts, for both sides.

`--html FILE` runs the same deterministic simulation and writes a self-contained, animated replay. Open the generated HTML file in a browser to inspect unit movement and combat alongside the agent/tool activity, collective threat consensus, supply chain, spiking-hive response, directives, and battle events. Use the playback controls to pause, seek, or change replay speed. Add `--script examples/wargames/orders.txt` to include human direction in the replay.

## Difficulty

At the default difficulty (2.5) the agents on their own lose the bunker at about 220 s. The same battle with `examples/wargames/orders.txt` (prefer spitters, build a tank, hand the rally point back to the agents) holds: about a dozen defenders lost, bunker at 90 percent. The outcome is sensitive to small changes, so treat it as a demonstration that human direction matters, not a benchmark. At difficulty 1 the agents hold unaided. `--selfcheck` asserts both results.
