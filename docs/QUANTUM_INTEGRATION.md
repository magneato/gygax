# Quantum technology integration boundary

Gygax has a vendor-neutral adapter contract for quantum-assisted navigation and
QKD-protected message channels in
[`include/gygax/quantum/adapters.hpp`](../include/gygax/quantum/adapters.hpp).
It does not include a quantum-device driver, quantum network, cryptographic
implementation, or quantum simulator.

## Navigation sensor adapter

Implement `gygax::quantum::NavigationSensor` to provide timestamped heading
estimates and uncertainty. A device-specific adapter is responsible for
converting its sensor outputs into the common estimate and representing
unavailable measurements as `std::nullopt`. Downstream navigation must retain
ordinary validity, uncertainty, freshness, and fallback checks; a quantum sensor
does not make an estimate infallible or provide a complete navigation solution.

## QKD provider adapter

Implement `gygax::quantum::QkdProvider` to establish a peer session and return a
`QkdSession` only when a provider-backed `AuthenticatedSecureChannel` is ready.
The session report records counts and whether authentication, error correction,
privacy amplification, and final key installation completed. Secret key bits
are deliberately not exposed through the report.

A production adapter must define and validate its protocol, peer identity,
authenticated classical channel, parameter-estimation and abort thresholds
(reported by `parameterEstimationAccepted`),
error correction, privacy amplification, key storage/rotation/destruction, and
message-channel protection. `AuthenticatedSecureChannel::send` accepts
plaintext and the provider implementation must protect it using the established
session; `QkdSession::ready()` is a structural completeness check, not a
security proof or certification. Never use a simulated report as authorization
to transmit a real command or sensitive message, and never silently downgrade a
failed quantum session to an unprotected channel.

QKD is not a replacement for authenticated classical communication: classical
messages used for basis comparison and protocol coordination must themselves be
authenticated. The adapter must clearly report failure and leave the protected
channel unavailable if authentication or protocol checks fail.

## Current simulation

The [Seek & Find demo](../examples/seek_and_find/README.md) uses a fixed,
deterministic mock heading estimate to select a fictional waypoint. Its HTML
also counts matching and discarded bases using a classical pseudorandom
illustration. It does not model quantum states, an eavesdropper, error
correction, privacy amplification, or a usable secret key. The storyboard
releases a simulated status event after its fictional link and basis-sifting
milestones; this is not cryptographic gating.

No vendor SDK or real quantum hardware has been selected or integrated. The
adapter contract is a starting point for a future implementation and must be
reviewed against the chosen provider's documented security and device model
before use.
