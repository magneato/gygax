#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

#include <gygax/sim/acoustics.hpp>

using namespace gygax::sim;

TEST(Acoustics, EqualOppositePhaseSourcesCancelAtTheSpecifiedPoint) {
    const std::array sources{
        AcousticSource{{0.0, 0.0}, 1000.0, 0.02, 0.0, 1.0},
        AcousticSource{{0.0, 0.0}, 1000.0, 0.02, std::numbers::pi, 1.0},
    };
    const auto sample = sampleCoherentTone({1.0, 0.0}, 1000.0, sources);
    EXPECT_LT(sample.rmsPressurePa(), 1e-12);
    EXPECT_LT(sample.splDb(), -200.0);
}

TEST(Acoustics, ReportsSingleSourceFreeFieldLevelAtReferenceDistance) {
    const std::array sources{AcousticSource{{0.0, 0.0}, 1000.0, 0.02, 0.0, 1.0}};
    const auto sample = sampleCoherentTone({1.0, 0.0}, 1000.0, sources);
    EXPECT_NEAR(sample.rmsPressurePa(), 0.02, 1e-12);
    EXPECT_NEAR(sample.splDb(), 60.0, 1e-9);
}

TEST(Acoustics, RejectsMixedFrequenciesRatherThanPretendingTheyAreCoherent) {
    const std::array sources{AcousticSource{{0.0, 0.0}, 440.0, 0.02, 0.0, 1.0}};
    EXPECT_THROW((void)sampleCoherentTone({1.0, 0.0}, 880.0, sources), std::invalid_argument);
}

TEST(Acoustics, ModulatedToneHasBoundedAmplitudeEnvelope) {
    const ToneSignal signal{1.0, 0.02, 1.0, 0.5};
    EXPECT_NEAR(sampleTone(signal, 0.25), 0.03, 1e-12);
    EXPECT_NEAR(sampleTone(signal, 0.75), -0.01, 1e-12);
}

TEST(Acoustics, RejectsInvalidModulationDepth) {
    const ToneSignal signal{880.0, 0.02, 2.0, 1.1};
    EXPECT_THROW((void)sampleTone(signal, 0.25), std::invalid_argument);
}
