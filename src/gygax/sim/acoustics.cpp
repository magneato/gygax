#include <gygax/sim/acoustics.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace gygax::sim {
namespace {

constexpr double kFrequencyToleranceHz = 1.0e-9;

void requireFinite(double value, const char* name) {
    if (!std::isfinite(value)) throw std::invalid_argument(name);
}

} // namespace

double AcousticFieldSample::rmsPressurePa() const {
    return std::hypot(inPhasePressurePa, quadraturePressurePa);
}

double AcousticFieldSample::splDb() const {
    const double pressure = rmsPressurePa();
    if (pressure == 0.0) return -std::numeric_limits<double>::infinity();
    return 20.0 * std::log10(pressure / kReferenceSoundPressurePascals);
}

AcousticFieldSample sampleCoherentTone(Vec2 observationPoint, double frequencyHz, std::span<const AcousticSource> sources) {
    requireFinite(observationPoint.x, "observation x must be finite");
    requireFinite(observationPoint.y, "observation y must be finite");
    requireFinite(frequencyHz, "frequency must be finite");
    if (frequencyHz <= 0.0) throw std::invalid_argument("frequency must be positive");

    AcousticFieldSample sample;
    const double waveNumber = 2.0 * std::numbers::pi * frequencyHz / kSpeedOfSoundMetersPerSecond;
    for (const auto& source : sources) {
        requireFinite(source.position.x, "source x must be finite");
        requireFinite(source.position.y, "source y must be finite");
        requireFinite(source.frequencyHz, "source frequency must be finite");
        requireFinite(source.rmsPressureAtReferencePa, "source pressure must be finite");
        requireFinite(source.phaseRadians, "source phase must be finite");
        requireFinite(source.referenceDistanceMeters, "reference distance must be finite");
        if (std::abs(source.frequencyHz - frequencyHz) > kFrequencyToleranceHz)
            throw std::invalid_argument("all coherent sources must match the requested frequency");
        if (source.rmsPressureAtReferencePa < 0.0 || source.referenceDistanceMeters <= 0.0)
            throw std::invalid_argument("source pressure must be nonnegative and reference distance positive");

        const double distance = std::hypot(observationPoint.x - source.position.x, observationPoint.y - source.position.y);
        const double propagatedPressure = source.rmsPressureAtReferencePa * source.referenceDistanceMeters /
                                          std::max(distance, source.referenceDistanceMeters);
        const double phase = source.phaseRadians - waveNumber * distance;
        sample.inPhasePressurePa += propagatedPressure * std::cos(phase);
        sample.quadraturePressurePa += propagatedPressure * std::sin(phase);
    }
    return sample;
}

double sampleTone(const ToneSignal& signal, double timeSeconds) {
    requireFinite(signal.carrierFrequencyHz, "carrier frequency must be finite");
    requireFinite(signal.peakPressurePa, "peak pressure must be finite");
    requireFinite(signal.modulationFrequencyHz, "modulation frequency must be finite");
    requireFinite(signal.modulationDepth, "modulation depth must be finite");
    requireFinite(timeSeconds, "sample time must be finite");
    if (signal.carrierFrequencyHz <= 0.0 || signal.modulationFrequencyHz < 0.0 || signal.peakPressurePa < 0.0 ||
        signal.modulationDepth < 0.0 || signal.modulationDepth > 1.0)
        throw std::invalid_argument("tone frequencies and pressure must be nonnegative; modulation depth must be in [0, 1]");

    const double modulationPhase = 2.0 * std::numbers::pi * signal.modulationFrequencyHz * timeSeconds;
    const double carrierPhase = 2.0 * std::numbers::pi * signal.carrierFrequencyHz * timeSeconds;
    const double envelope = 1.0 + signal.modulationDepth * std::sin(modulationPhase);
    return signal.peakPressurePa * envelope * std::sin(carrierPhase);
}

} // namespace gygax::sim
