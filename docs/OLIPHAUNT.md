# Operation Oliphaunt: Never Forgets

Oliphaunt keeps a local history of sightings that would otherwise be easy to forget. It can distinguish a recurring
entity seen across unrelated places and days from ordinary repeated traffic at one location. It is a memory and
patterning tool, not a detector or a claim that someone is following you.

## Privacy and boundaries

- Identifiers such as a plate or device ID are salted and hashed on arrival. The original identifier is not written to
  the memory file. Findings show a short hash prefix so a person can trust or erase one entry without exposing the ID.
- Labels should describe a non-identifying appearance (for example, "grey hatchback"), not a person's name or other
  sensitive details.
- Sightings, place labels, and approximate coordinates remain in the configured local memory. The service does not
  forward them to another node on its own; authorized API callers and model-driven agents may receive tool output when
  they explicitly invoke these tools.
- The Quest camera feature is experimental, opt-in, and separate from the memory engine. It only counts camera frames;
  it does not recognize plates, faces, vehicles, or people, save frames, or submit sightings.
- Review findings with context. Repeated proximity alone does not prove intent or danger. Trusted entries can be
  suppressed, safe places can be excluded, and entries can be forgotten or erased entirely.

## Concern levels

The engine groups nearby sightings into encounters, clusters locations, and counts distinct days and locations outside
safe places. It escalates from `noted` through `notice` and `alert` to `persistent` as the record accumulates. The
default memory retention is 180 days; set `retentionDays` to `0` to keep entries indefinitely. Safe-place sightings
are excluded from the location/day measures used for following assessments.

An observation with an empty ID or invalid coordinate is rejected. Old sightings outside the retention window are
rejected rather than silently treated as current. The persistent store is written atomically with owner-only file
permissions. If an existing store is corrupt, it is preserved as `.corrupt` and an error is reported.

## CLI

The CLI's default store is `~/.local/share/gygax/oliphaunt.json`; override it with `GYGAX_OLIPHAUNT_PATH` or
`--file PATH`. For `gygax serve`, use the same environment variable or `--oliphaunt PATH`.

```bash
./build/oliphaunt demo
./build/oliphaunt observe vehicle DEMO-ENTITY --label "grey hatchback" --place "north road"
./build/oliphaunt report
./build/oliphaunt trust vehicle DEMO-ENTITY
./build/oliphaunt forget vehicle DEMO-ENTITY
./build/oliphaunt forget --all
```

The raw ID is accepted only as command input so it can be hashed. The report and persistent file contain only the hash
prefix; CLI trust/forget commands take the same original identifier, while the headset uses the prefix. Keep the data
file private and choose location labels that do not reveal more than needed.

## Service tools

Set `GYGAX_OLIPHAUNT_PATH` or pass `--oliphaunt PATH` to persist the service memory; without a path the service keeps
memory in RAM. The standalone CLI defaults to `~/.local/share/gygax/oliphaunt.json`. The service registers:

| Tool | Purpose |
| --- | --- |
| `oliphaunt.observe` | Add one observation with an identifier, kind, optional non-identifying label, place or coordinates, and time |
| `oliphaunt.report` | Return findings at or above the requested concern level |
| `oliphaunt.trust` | Suppress an entity from reports |
| `oliphaunt.forget` | Erase one entity or all memory |
| `oliphaunt.safe_place` | Add a named safe place with optional coordinates and radius |

## glyder AR controls

The AR panel can request a report, trust an entity, or forget it by its short ID. The host must be running
`gygax serve --glyder`; its memory is the service memory configured above. The Quest view uses `XR_FB_passthrough`
when the runtime provides it and supports alpha blending. Base mode disables AR and passthrough but leaves spectacle
correction and colour-vision settings unchanged.
