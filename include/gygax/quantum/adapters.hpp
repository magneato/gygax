#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace gygax::quantum {

struct NavigationEstimate {
    double headingRadians = 0.0;
    double headingUncertaintyRadians = 0.0;
    std::chrono::steady_clock::time_point sampledAt{};
};

class NavigationSensor {
public:
    virtual ~NavigationSensor() = default;
    [[nodiscard]] virtual std::optional<NavigationEstimate> sample() = 0;
};

struct QkdSiftingReport {
    std::size_t signalsSent = 0;
    std::size_t siftedBits = 0;
    std::size_t parameterSampleBits = 0;
    std::size_t observedErrors = 0;
    std::size_t finalKeyBits = 0;
    bool classicalChannelAuthenticated = false;
    bool parameterEstimationAccepted = false;
    bool errorCorrectionComplete = false;
    bool privacyAmplificationComplete = false;
};

class AuthenticatedSecureChannel {
public:
    virtual ~AuthenticatedSecureChannel() = default;
    // Implementations protect plaintext with the established session keys.
    [[nodiscard]] virtual bool send(std::span<const std::byte> plaintext) = 0;
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> receive() = 0;
};

struct QkdSession {
    QkdSiftingReport report;
    std::unique_ptr<AuthenticatedSecureChannel> channel;

    [[nodiscard]] bool ready() const noexcept {
        return channel != nullptr && report.siftedBits <= report.signalsSent && report.parameterSampleBits > 0 &&
               report.parameterSampleBits <= report.siftedBits && report.observedErrors <= report.parameterSampleBits &&
               report.finalKeyBits <= report.siftedBits - report.parameterSampleBits && report.finalKeyBits > 0 &&
               report.classicalChannelAuthenticated && report.parameterEstimationAccepted && report.errorCorrectionComplete &&
               report.privacyAmplificationComplete;
    }
};

class QkdProvider {
public:
    virtual ~QkdProvider() = default;

    [[nodiscard]] virtual std::optional<QkdSession> establishSession(std::span<const std::byte> peerIdentity) = 0;
};

} // namespace gygax::quantum
