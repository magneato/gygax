#include <gtest/gtest.h>

#include <gygax/quantum/adapters.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace {

class SimulatedNavigationSensor final : public gygax::quantum::NavigationSensor {
public:
    std::optional<gygax::quantum::NavigationEstimate> sample() override {
        return gygax::quantum::NavigationEstimate{
            .headingRadians = 0.25,
            .headingUncertaintyRadians = 0.01,
            .sampledAt = std::chrono::steady_clock::time_point{},
        };
    }
};

class SimulatedSecureChannel final : public gygax::quantum::AuthenticatedSecureChannel {
public:
    bool send(std::span<const std::byte> plaintext) override {
        lastSent.assign(plaintext.begin(), plaintext.end());
        return true;
    }

    std::optional<std::vector<std::byte>> receive() override {
        return lastSent;
    }

    std::vector<std::byte> lastSent;
};

class SimulatedQkdProvider final : public gygax::quantum::QkdProvider {
public:
    std::optional<gygax::quantum::QkdSession> establishSession(
        std::span<const std::byte>) override {
        using namespace gygax::quantum;
        return QkdSession{
            .report = {
                .signalsSent = 64,
                .siftedBits = 32,
                .parameterSampleBits = 8,
                .observedErrors = 0,
                .finalKeyBits = 16,
                .classicalChannelAuthenticated = true,
                .parameterEstimationAccepted = true,
                .errorCorrectionComplete = true,
                .privacyAmplificationComplete = true,
            },
            .channel = std::make_unique<SimulatedSecureChannel>(),
        };
    }
};

TEST(QuantumAdapters, NavigationSensorProvidesTimestampedEstimate) {
    SimulatedNavigationSensor sensor;

    const auto estimate = sensor.sample();

    ASSERT_TRUE(estimate.has_value());
    EXPECT_DOUBLE_EQ(estimate->headingRadians, 0.25);
    EXPECT_DOUBLE_EQ(estimate->headingUncertaintyRadians, 0.01);
}

TEST(QuantumAdapters, QkdSessionRequiresAllPostProcessingAndSecureChannel) {
    using namespace gygax::quantum;

    QkdSession session{
        .report = {
            .signalsSent = 64,
            .siftedBits = 32,
            .parameterSampleBits = 8,
            .observedErrors = 0,
            .finalKeyBits = 16,
            .classicalChannelAuthenticated = true,
            .parameterEstimationAccepted = true,
            .errorCorrectionComplete = true,
            .privacyAmplificationComplete = true,
        },
        .channel = std::make_unique<SimulatedSecureChannel>(),
    };
    EXPECT_TRUE(session.ready());

    session.report.privacyAmplificationComplete = false;
    EXPECT_FALSE(session.ready());
    session.report.privacyAmplificationComplete = true;
    session.report.parameterSampleBits = 0;
    EXPECT_FALSE(session.ready());
    session.report.parameterSampleBits = 8;
    session.report.observedErrors = session.report.parameterSampleBits + 1;
    EXPECT_FALSE(session.ready());
}

TEST(QuantumAdapters, SiftedReportRejectsImpossibleCounts) {
    using namespace gygax::quantum;

    QkdSession session{
        .report = {
            .signalsSent = 16,
            .siftedBits = 17,
            .parameterSampleBits = 4,
            .observedErrors = 0,
            .finalKeyBits = 8,
            .classicalChannelAuthenticated = true,
            .parameterEstimationAccepted = true,
            .errorCorrectionComplete = true,
            .privacyAmplificationComplete = true,
        },
        .channel = std::make_unique<SimulatedSecureChannel>(),
    };

    EXPECT_FALSE(session.ready());
}

TEST(QuantumAdapters, ProviderReturnsSessionWithProtectedChannelContract) {
    SimulatedQkdProvider provider;
    const std::array peerIdentity{std::byte{0x01}};

    auto session = provider.establishSession(peerIdentity);

    ASSERT_TRUE(session.has_value());
    EXPECT_TRUE(session->ready());
}

TEST(QuantumAdapters, SecureChannelUsesOpaqueBytePayloads) {
    SimulatedSecureChannel channel;
    const std::array payload{std::byte{0x47}, std::byte{0x58}};

    ASSERT_TRUE(channel.send(payload));
    const auto received = channel.receive();

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, std::vector<std::byte>(payload.begin(), payload.end()));
}

} // namespace
