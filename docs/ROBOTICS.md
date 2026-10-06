# Robotics, vehicles, buses and aircraft

Gygax talks to physical and simulated machines through **devices**. A device is a named connection to one machine or bus, created through the service (`/v1/devices`, JSON-RPC `devices.*`) and exposed to agents as tools.

## Protocols

| Kind | Talks to | Link | Notes |
| --- | --- | --- | --- |
| `mavlink` | ArduPilot, PX4 drones, rovers, boats, planes | `udp://`, `tcp://`, `serial://` | MAVLink v1 and v2, 205 common-dialect messages generated from pymavlink, CRC_EXTRA verified, arm, mode, takeoff, land, RTL, goto, message rates, parameters |
| `can` | Any CAN or CAN FD bus | `"interface":"can0"` (SocketCAN, Linux) or `virtual:name` (in-process bus) | ISO-TP, J1939, DBC decoding, CANopen |
| `obd` | Cars and trucks (OBD-II) | CAN | Mode 01/03/09 polling, DTC decoding |
| `modbus` | PLCs, drives, industrial robots | `tcp://`, `serial://` | RTU and TCP, named register maps |
| `nmea` | GPS, marine instruments | `serial://`, `udp://`, `tcp://` | NMEA 0183 with checksum validation |
| `adsb` | Aircraft receivers | `tcp://`, `udp://` | Mode S with CRC-24, CPR position decoding, per-aircraft tracker |
| `rosbridge` | ROS and ROS 2 robots | `tcp://` (WebSocket) | rosbridge v2 protocol: subscribe, publish, service calls |

ARINC 429 word encoding and decoding is available as a library (`gygax/bus/arinc429.hpp`) for avionics work.

`memory://name` links exist for tests and simulations.

The separate [quantum integration boundary](QUANTUM_INTEGRATION.md) defines
vendor-neutral interfaces for navigation estimates and QKD-protected channels.
No quantum sensor or QKD hardware driver is currently included.

## Trying it without hardware

```bash
gygax sim-autopilot                          # virtual multicopter, MAVLink on udp://127.0.0.1:14550
gygax serve --device-commands &
curl -X POST localhost:1984/v1/devices -d '{"id":"uav","kind":"mavlink","uri":"udp://0.0.0.0:14550"}'
curl localhost:1984/v1/devices/uav/state
curl -X POST localhost:1984/v1/devices/uav/command -d '{"command":"arm"}'
```

The simulator answers the same MAVLink commands a real autopilot does and refuses what a real one refuses (takeoff before arming, disarm in flight). It also works with pymavlink and QGroundControl-style listeners on port 14550.

## Safety model

Commands move real machines, so:

- Reading state is always allowed. Sending commands is **off** unless the service starts with `--device-commands` or `GYGAX_DEVICE_COMMANDS=1`. Without it the `device.command` tool is not even registered.
- Serial device paths must be under `/dev/`.
- Device ids are 1 to 64 characters of `[A-Za-z0-9._-]`; at most 64 devices.
- A non-loopback bind requires an API token.
- Gygax adds no safety layer to the vehicle itself. Keep the autopilot's own failsafes, geofence and a hardware kill switch.
- Enabling `--device-commands` is an operator/deployment choice, not a per-command human approval gate. Gygax does not provide that gate; add and independently verify one if the use case requires it. See [public API and trust boundaries](SDK_API.md).

## Verification

Wire formats are checked against reference implementations: MAVLink frames byte for byte against pymavlink, DBC and J1939 identifiers against cantools, ADS-B against pyModeS, published CRC and checksum vectors. SocketCAN is exercised only where a `vcan0` interface exists (the test skips otherwise; CI creates one).

## Not covered

LIN, FlexRay, MIL-STD-1553, AFDX, MAVLink message signing, and vendor-specific dialects beyond the common message set.
