# Seek & Find: simulation demo

This standalone fictional demo illustrates simulated scout coordination,
communications recovery, a mock quantum-inertial heading estimate, QKD basis
sifting, UV-visible prop discovery, and a benign inspection drone following
visual breadcrumbs.

## Open the demo

- Open [`seek_and_find.html`](seek_and_find.html) in a browser. Playback, pause,
  step, and seek controls are built into the page.
- Open [`seek_and_find.usda`](seek_and_find.usda) in a USD-compatible scene
  viewer and scrub frames 0 to 240 to inspect the animated scene.

## Scope and safety boundary

This is a visual simulation, not a live Gygax service, radio/network test, GPS
demonstration, machine-learning training run, or hardware-control interface. The
relay links and UV marking are explanatory scene elements; no external
communication or sensor APIs are accessed. The USD inspection drone and visual
waypoints are fictional props, not a tested navigation or tracking method.

The navigation estimate is deterministic mock data used to choose a simulated
waypoint. The QKD panel is only a classical, deterministic illustration of
comparing bases and discarding mismatches. It does not simulate quantum
measurement security, authenticate a classical channel, estimate or correct
errors, perform privacy amplification, distribute a secret key, encrypt
messages, or authorize real activities. The displayed storyboard gate is not a
cryptographic control.

Games, races, and control policies may be explored in simulation. Simulation
results alone do not establish that a policy or machine is safe for the real
world. Real-machine trials require separate authorization, independent safety
review, hardware-in-the-loop validation, and controlled test conditions.

## Vendor-neutral adapter boundary

[`include/gygax/quantum/adapters.hpp`](../../include/gygax/quantum/adapters.hpp)
defines interfaces for navigation estimates and QKD sessions with an
authenticated secure channel. No quantum sensor or QKD provider implementation
is included. A future vendor adapter must implement its device-specific
authentication, parameter estimation, error correction, privacy amplification,
key lifecycle, and protected channel; the simulation cannot stand in for that
work. See [Quantum integration](../../docs/QUANTUM_INTEGRATION.md).
