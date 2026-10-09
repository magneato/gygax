#include <gtest/gtest.h>

#include <gygax/satlink/sdr.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <numbers>
#include <random>
#include <thread>
#include <vector>

using namespace gygax::satlink;

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

std::vector<Iq> tone(double hz, double rate, std::size_t n, double amplitude = 1.0, double noise = 0.0, unsigned seed = 1) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, noise);
    std::vector<Iq> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        const double a = kTwoPi * hz * static_cast<double>(k) / rate;
        out[k] = Iq(static_cast<float>(amplitude * std::cos(a) + g(rng)), static_cast<float>(amplitude * std::sin(a) + g(rng)));
    }
    return out;
}

} // namespace

TEST(SatLinkDsp, FftMatchesTheDirectTransform) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (std::size_t n : {2u, 8u, 64u, 256u}) {
        std::vector<std::complex<double>> x(n);
        for (auto& v : x) v = {u(rng), u(rng)};
        auto fast = x;
        fft(fast);
        for (std::size_t k = 0; k < n; ++k) {
            std::complex<double> direct = 0.0;
            for (std::size_t j = 0; j < n; ++j)
                direct += x[j] * std::polar(1.0, -kTwoPi * static_cast<double>(j * k) / static_cast<double>(n));
            EXPECT_NEAR(std::abs(fast[k] - direct), 0.0, 1e-9) << "n=" << n << " k=" << k;
        }
    }
    std::vector<std::complex<double>> bad(12);
    EXPECT_THROW(fft(bad), std::invalid_argument);
}

TEST(SatLinkDsp, SpectrumFindsAToneBetweenBins) {
    constexpr double rate = 250'000.0;
    for (double hz : {-61'234.5, -1'000.0, 0.0, 17'777.7, 99'000.0}) {
        const auto iq = tone(hz, rate, 16 * 4096, 0.5, 0.05);
        const auto spectrum = powerSpectrumDb(iq, 4096);
        const auto peak = strongestPeak(spectrum, rate);
        EXPECT_NEAR(peak.offset_hz, hz, 0.25 * rate / 4096) << hz; // within a quarter bin (15 Hz)
        EXPECT_NEAR(peak.power_db, 20.0 * std::log10(0.5), 1.6);   // Hann scalloping is at most 1.42 dB
        EXPECT_GT(peak.snr_db, 30.0);
    }
}

TEST(SatLinkDsp, U8SamplesAreCentred) {
    const std::uint8_t bytes[] = {0, 255, 127, 128};
    const auto iq = iqFromU8(bytes);
    ASSERT_EQ(iq.size(), 2u);
    EXPECT_NEAR(iq[0].real(), -1.0f, 1e-6);
    EXPECT_NEAR(iq[0].imag(), 1.0f, 1e-6);
    EXPECT_NEAR(iq[1].real(), -0.5f / 127.5f, 1e-6);
    EXPECT_NEAR(iq[1].imag(), 0.5f / 127.5f, 1e-6);
}

// A carrier whose Doppler sweeps from +9 kHz to -9 kHz, as over an overhead ISS pass at 437.8 MHz
// compressed into one second, comes out of the mixer as a steady tone at DC, phase continuous
// across blocks.
TEST(SatLinkDsp, DopplerMixerRemovesASweepingCarrier) {
    constexpr double rate = 96'000.0;
    constexpr std::size_t n = 96'000, block = 4'800;
    const auto f = [&](double t) { return 9'000.0 - 18'000.0 * t; }; // Hz at time t seconds
    std::vector<Iq> iq(n);
    for (std::size_t k = 0; k < n; ++k) {
        const double t = static_cast<double>(k) / rate;
        double turns = 9'000.0 * t - 9'000.0 * t * t; // the integral of f
        turns -= std::floor(turns);
        iq[k] = std::polar(1.0f, static_cast<float>(kTwoPi * turns));
    }
    DopplerMixer mixer(rate);
    for (std::size_t b = 0; b < n; b += block) {
        const double t0 = static_cast<double>(b) / rate, t1 = static_cast<double>(b + block) / rate;
        mixer.apply(std::span(iq).subspan(b, block), f(t0), f(t1));
    }
    // What remains is a constant phasor: every sample equal to the first.
    for (std::size_t k = 0; k < n; k += 997) EXPECT_LT(std::abs(iq[k] - iq[0]), 2e-3f) << k;
    const auto peak = strongestPeak(powerSpectrumDb(iq, 8192), rate);
    EXPECT_NEAR(peak.offset_hz, 0.0, rate / 8192);
}

namespace {

// A stand-in rtl_tcp server: greets like an R820T dongle, records commands, and streams a tone
// at a given offset from whatever it is tuned to, as unsigned 8-bit IQ.
class FakeRtlTcp {
public:
    explicit FakeRtlTcp(double tone_offset_hz, double rate) : offset_(tone_offset_hz), rate_(rate) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(fd_, 1);
        socklen_t len = sizeof(addr);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeRtlTcp() {
        stop_ = true;
        ::shutdown(fd_, SHUT_RDWR);
        if (const int c = client_.load(); c >= 0) ::shutdown(c, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        ::close(fd_);
    }
    [[nodiscard]] int port() const { return port_; }
    std::vector<std::pair<int, std::uint32_t>> commands() {
        stop_ = true;
        if (const int c = client_.load(); c >= 0) ::shutdown(c, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        return commands_;
    }

private:
    void serve() {
        const int c = ::accept(fd_, nullptr, nullptr);
        if (c < 0) return;
        client_ = c;
        const std::uint8_t greeting[12] = {'R', 'T', 'L', '0', 0, 0, 0, 5, 0, 0, 0, 29};
        ::send(c, greeting, sizeof(greeting), MSG_NOSIGNAL);
        std::vector<std::uint8_t> out(8192);
        double phase = 0.0;
        std::uint8_t cmd[5];
        std::size_t have = 0;
        while (!stop_) {
            // Commands first (non-blocking), then a block of samples.
            for (;;) {
                const auto n = ::recv(c, cmd + have, 5 - have, MSG_DONTWAIT);
                if (n <= 0) break;
                have += static_cast<std::size_t>(n);
                if (have == 5) {
                    commands_.emplace_back(cmd[0], (std::uint32_t{cmd[1]} << 24) | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4]);
                    have = 0;
                }
            }
            for (std::size_t i = 0; i < out.size(); i += 2) {
                out[i] = static_cast<std::uint8_t>(std::lround(127.5 + 100.0 * std::cos(kTwoPi * phase)));
                out[i + 1] = static_cast<std::uint8_t>(std::lround(127.5 + 100.0 * std::sin(kTwoPi * phase)));
                phase += offset_ / rate_;
                phase -= std::floor(phase);
            }
            if (::send(c, out.data(), out.size(), MSG_NOSIGNAL) <= 0) break;
        }
        client_ = -1;
        ::close(c);
    }

    double offset_, rate_;
    int fd_{-1}, port_{0};
    std::atomic<int> client_{-1};
    std::atomic<bool> stop_{false};
    std::vector<std::pair<int, std::uint32_t>> commands_;
    std::thread thread_;
};

} // namespace

TEST(SatLinkRtlTcp, TunesAndCapturesThroughTheProtocol) {
    constexpr double rate = 1'024'000.0, offset = 37'500.0;
    std::vector<std::pair<int, std::uint32_t>> log;
    {
        FakeRtlTcp server(offset, rate);
        RtlTcpClient sdr("127.0.0.1", server.port(), 2000);
        EXPECT_EQ(sdr.info().tunerName(), "R820T");
        EXPECT_EQ(sdr.info().gain_stages, 29u);
        sdr.setSampleRate(static_cast<std::uint32_t>(rate));
        sdr.setCenterFrequency(437'800'000.4);
        sdr.setGainDb(40.2);
        sdr.setFrequencyCorrection(-3);
        sdr.discard(16'384);
        const auto iq = sdr.read(65'536);
        const auto peak = strongestPeak(powerSpectrumDb(iq, 8192), sdr.sampleRate());
        EXPECT_NEAR(peak.offset_hz, offset, rate / 8192 / 4);
        EXPECT_GT(peak.snr_db, 40.0);
        EXPECT_DOUBLE_EQ(sdr.centerFrequency(), 437'800'000.0);
        log = server.commands();
    }
    const std::vector<std::pair<int, std::uint32_t>> expected = {
        {0x02, 1'024'000}, {0x01, 437'800'000}, {0x03, 1}, {0x04, 402}, {0x05, static_cast<std::uint32_t>(-3)}};
    EXPECT_EQ(log, expected);
}

TEST(SatLinkRtlTcp, RejectsWhatIsNotRtlTcp) {
    // A server that greets with something else.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(fd, 1);
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    std::thread t([fd] {
        const int c = ::accept(fd, nullptr, nullptr);
        const char hello[] = "HTTP/1.1 200";
        ::send(c, hello, 12, MSG_NOSIGNAL);
        ::close(c);
    });
    EXPECT_THROW(RtlTcpClient("127.0.0.1", ntohs(addr.sin_port), 1000), SdrError);
    t.join();
    ::close(fd);
    EXPECT_THROW(RtlTcpClient("127.0.0.1", 0), SdrError);
}
