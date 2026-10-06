#pragma once

#include <span>

#include <gygax/sim/world2d.hpp>

namespace gygax::sim {

inline constexpr double kSpeedOfSoundMetersPerSecond = 343.0;
inline constexpr double kReferenceSoundPressurePascals = 20.0e-6;

/**
 * A coherent, monochromatic point source for a simplified 2D sound-field
 * estimate. Pressure is RMS at referenceDistanceMeters in free-field space.
 * This is a simulation value only; it is not a speaker or flight command.
 */
struct AcousticSource {
    Vec2 position;
    double frequencyHz = 1000.0;
    double rmsPressureAtReferencePa = 0.02;
    double phaseRadians = 0.0;
    double referenceDistanceMeters = 1.0;
};

/** In-phase and quadrature RMS pressure components at one observation point. */
struct AcousticFieldSample {
    double inPhasePressurePa = 0.0;
    double quadraturePressurePa = 0.0;

    [[nodiscard]] double rmsPressurePa() const;
    [[nodiscard]] double splDb() const;
};

/**
 * Estimate the coherent sum of equal-frequency point sources at one point.
 * The result is location- and frequency-specific. The model omits barriers,
 * reflections, air absorption, source directivity, wind, and background noise.
 */
[[nodiscard]] AcousticFieldSample sampleCoherentTone(Vec2 observationPoint, double frequencyHz, std::span<const AcousticSource> sources);

/**
 * An explicitly synthetic amplitude-modulated tone for simulation and
 * notification-design experiments. It does not synthesize speech, imitate
 * another source, or connect to an audio device.
 */
struct ToneSignal {
    double carrierFrequencyHz = 880.0;
    double peakPressurePa = 0.02;
    double modulationFrequencyHz = 2.0;
    double modulationDepth = 0.25;
};

/** Return one instantaneous pressure sample, or throw for invalid parameters. */
[[nodiscard]] double sampleTone(const ToneSignal& signal, double timeSeconds);

} // namespace gygax::sim
