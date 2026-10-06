# Supply chain logistics

Products and information flow together: every unit that enters or leaves the fleet is a ledger event, every unit carries a GUID, and the same service that moves the drones plans their routes and reports the flow.

## Concepts

| Concept | Meaning |
| --- | --- |
| **Model** | A SKU with capabilities: `kind`, `power`, `max_range_m`, `cruise_mps`. Range-aware planning reads it. |
| **Unit** | One physical item with a GUID (UUID v4), a SKU, attributes, a status (`active`, `lost`, `retired`), an optional bound `device`, `site`, `fuel` (0..1) and last position. |
| **Event** | An append-only record: signed count, SKU, attributes, the GUIDs it touched, reason, site, note, time. |
| **Site** | A factory, depot, hub, charge point or refuel point with a position and the power types it serves. |

## The line format

```
-3 ba99x drone=quadcopter power=solar        three units lost
+8 ba99x drone=quadcopter site=plant-1       the factory produced eight
-1 ba99x guid=6f9619ff-8b86-4011-b42d-00c04fc964ff reason=crash
```

The first token is a signed non-zero count, the second the SKU, then `key=value` pairs. `guid=` (comma separated), `reason=` and `site=` are reserved; everything else is an attribute. Tokens use `[A-Za-z0-9._:/-]`, 64 characters at most, at most 16 attributes.

Rules:

- **Positive** counts create units. Without `guid=` the service generates the GUIDs and returns them. With `guid=` the count must match, and a GUID that belongs to a lost or retired unit of the same SKU is reactivated (recovery). Reason defaults to `produced`.
- **Negative** counts end units. With `guid=` exactly those units are ended. Without it the oldest active units of that SKU whose attributes include the given ones are chosen. If fewer exist the request fails with `409 insufficient_stock` and the count available. Reason `lost` (the default) marks units `lost`; any other reason marks them `retired`.
- Nothing is ever deleted. A loss is an event you can audit.

## HTTP API

All under `/v1/logistics`, all also JSON-RPC methods named `logistics.<area>.<op>`.

| Request | Purpose |
| --- | --- |
| `POST /models` | `{"sku","kind","power","max_range_m","cruise_mps","attrs"}` |
| `GET /models` | List |
| `POST /sites` | `{"id","name","kind","latitude","longitude","services":["solar","battery"]}` |
| `GET /sites`, `DELETE /sites/{id}` | List, remove |
| `POST /events` | `{"line":"-3 ba99x drone=quadcopter"}` or `{"delta","sku","attrs","guids","reason","site","note"}` |
| `GET /events?sku=&since=&limit=` | History with the compact `line` for each event |
| `GET /units?sku=&status=&limit=` | Units |
| `GET /units/{guid}` | One unit |
| `POST /units/{guid}` | Update `device`, `site`, `fuel`, `latitude` and `longitude` |
| `GET /summary?since=7d` | Report described below |
| `POST /plan` | Range-aware route planning |

`since` accepts epoch seconds or milliseconds, durations (`90m`, `24h`, `7d`, `2w`) and ISO dates (`2026-09-01`, `2026-09-01T00:00:00Z`).

Agents get the tools `logistics.record`, `logistics.summary`, `logistics.plan` and `logistics.units`.

The summary answers "what did we lose and gain this week":

```json
{
  "totals": {"active": 5, "lost": 3, "retired": 0},
  "skus": [{"sku": "ba99x", "active": 5, "lost": 3, "gained_in_window": 8, "lost_in_window": 3}],
  "flow": [
    {"line": "+8 ba99x drone=quadcopter power=solar", "delta": 8, "events": 1},
    {"line": "-3 ba99x drone=quadcopter power=solar", "delta": -3, "events": 1}
  ]
}
```

`GET /metrics` exports `gygax_logistics_units{sku,status}` and `gygax_logistics_events`.

## Range-aware planning

`POST /v1/logistics/plan` sends a unit or SKU across waypoints without exceeding its range.

```json
{"unit": "6f9619ff-8b86-4011-b42d-00c04fc964ff",
 "waypoints": [{"latitude": 47.5, "longitude": 8.9}],
 "return_home": true}
```

Inputs come from, in increasing priority: the model (`max_range_m`, `cruise_mps`, `power`), the unit (`fuel`, last position), the bound device (live battery percentage and position when the device is connected), and the request (`origin`, `origin_site`, `max_range_m`, `fuel` or `fuel_percent`, `reserve`, `power`, `return_to`, `return_home`). `inputs.fuel_source` in the reply says which one won.

The planner finds the shortest chain through the registered sites that never asks the vehicle to fly further than its usable range (range minus a reserve, 10 percent by default). Sites whose `services` do not include the vehicle's power type are ignored. Arrival at an intermediate waypoint must leave enough fuel to reach a refuel site or the next waypoint, so a route is never accepted that strands the vehicle after the first leg.

A feasible reply lists the legs, `refuel_stops`, `total_distance_m`, `eta_seconds`, `final_fuel_fraction` and a flat `route` of positions ready to send as goto commands to a device. An infeasible reply has `feasible: false`, the reason, the blocked waypoint and `suggestion.add_refuel_point`: the furthest point the vehicle can reach toward the blocked waypoint, which is where a new site would unblock the route. Register it with `POST /sites` and plan again.

The planner does not model wind, payload, terrain or battery aging, and distances are great-circle. Treat the reserve as your safety margin and set it for your fleet.

## Durability

With `--ledger PATH` (or `GYGAX_LEDGER_PATH`) every mutation is appended to a JSON-lines journal and `fsync`ed before it is applied. On start the journal is replayed. A torn final line from a crash is dropped; corruption anywhere else stops the service with an error instead of silently losing history. The file is created with mode 0600. Back it up like any database; rotate by starting a new file from a summary if it grows too large (10,000 events is roughly 3 MB).

Without a journal the ledger lives in memory and is lost on restart. Limits: 1,000,000 units, 4,096 models, 256 sites, 100,000 units per event.

## Trust

Anyone with the API token can record events; agents with the logistics tools can too. The ledger is an audit trail of what the service was told, not proof that a drone crashed. Wire it to real telemetry (a lost heartbeat on a bound device, for example) in your own automation, and keep the token secret.
