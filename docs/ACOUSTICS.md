# Acoustic simulation and audible signaling

Gygax includes an in-tree **simulation-only** acoustic model for studying
frequency-specific interference at defined measurement points and for
sampling a synthetic amplitude-modulated tone. The experimental header is not
currently part of the installed SDK. It has no audio output, microphone,
speaker, flight-control, or device-command integration.

## Coherent point-source estimate

`gygax/sim/acoustics.hpp` sums equal-frequency source pressures as phase-aware
in-phase and quadrature components at one 2D observation point. Pressure
decreases with distance using a capped inverse-distance estimate; the returned
SPL is relative to 20 μPa. The default sound speed is 343 m/s. Configure the
source phase and reference pressure explicitly.

This is a deliberately small teaching and comparison model, not an acoustic
solver. It omits obstacles, reflections, room and ground effects, air
absorption, source directivity, wind, moving-source Doppler effects, changing
phase synchronization, and background noise. It assumes stationary
monochromatic sources and a stationary measurement point. It cannot predict a
neighborhood's real sound exposure or establish compliance with noise limits.

Destructive interference is specific to frequency, phase, and location. A
reduction calculated at one point does not imply a reduction elsewhere; another
location can receive more sound. Do not interpret a low modeled level as
inaudibility, privacy, or concealment.

## Synthetic modulation

`sampleTone()` produces samples for one synthetic sinusoidal carrier with
bounded sinusoidal amplitude modulation. It is suitable for exploring
recognizable, attributable notification patterns in a simulation or offline
analysis. It does not synthesize speech or music and does not provide a path to
an audio device. Do not use sound imitation to misrepresent a drone's identity
or source.

## Responsible delivery-fleet studies

For delivery-noise work, compare source-level reductions, operating schedules,
and estimated exposure at declared measurement points. Report assumptions and
uncertainty, and measure real equipment with calibrated instrumentation and
appropriate community notice and consent. Prefer reducing emitted sound at its
source over attempting to cancel it at selected locations. Any deployment of
audible signals should be lawful, clearly attributable, and reviewed for
hearing exposure and local requirements.

Example:

```cpp
#include <array>
#include <gygax/sim/acoustics.hpp>

using namespace gygax::sim;

const std::array emitters{
    AcousticSource{{0.0, 0.0}, 500.0, 0.02, 0.0, 1.0},
    AcousticSource{{2.0, 0.0}, 500.0, 0.02, 1.2, 1.0},
};
const auto estimate = sampleCoherentTone({1.0, 3.0}, 500.0, emitters);
const double modeledLevelDb = estimate.splDb();
```

`modeledLevelDb` is only the output of the stated simplified model. It is not
an operational target, hardware command, or measured sound level.
