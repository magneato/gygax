# TL;DR

**What:** one C++26 binary for autonomous systems. It is a model router and agent runtime, a spiking-neuron
simulator, a satellite tracker (SatLink), and a robotics and hardware toolkit (MAVLink drones, CAN, Modbus, NMEA,
ADS-B, serial, GPIO).

**Whose cloud:** yours.
* No telemetry, no analytics, no update checks, no license server, no hosted service.
* It talks only to the model engines, peer nodes, MCP servers and devices **you** configure.
* It listens on `127.0.0.1` and refuses any other address until you set an API token.
* It builds offline (`-DGYGAX_BUILD_TESTS=OFF`). The browser extension can only reach `127.0.0.1:1984`.
* It's all source: read it, firewall it, `tcpdump` it.

**Models:** bring your own. Anything that speaks the standard `/v1/chat/completions` API works: Ollama, llama.cpp,
vLLM, LM Studio, or another Gygax node. The built-in `echo` engine needs no model and no network.

**Satellites:** `gygax track` predicts passes and Doppler from a TLE, retunes a radio through Hamlib `rigctld`, and
listens with an RTL-SDR over `rtl_tcp`.
Its SGP4/SDP4 matches the reference implementation's verification output to 0.12 mm. It never fetches elements
itself; you bring them.

**Drones and vehicles:** MAVLink with a virtual autopilot for testing. **Commands are off by default**; you turn on
authority explicitly.

**Try it:**
```bash
./setup.sh && ./build.sh && ./build/gygax doctor     # 21 self-tests, no network
./build/gygax serve                                   # local service on 127.0.0.1:1984
./build/gygax track --tle examples/satlink/sample.tle --at 51.48,0,46 --start 2024-01-01T12:00
```

**Trust it:** `ctest --test-dir build` runs 404 tests. `./scripts/dogfood.sh` adds sanitizer builds and a live
service smoke test.

**Licence:** Clear BSD (BSD-3-Clause-Clear), no patent rights granted. Personal, hobby, educational and
non-commercial research use is permitted; anything else needs a licence: [PATENTS.md](PATENTS.md).
