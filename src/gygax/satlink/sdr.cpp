// SatLink SDR: the rtl_tcp protocol, FFT spectra and Doppler removal.

#include <gygax/satlink/sdr.hpp>

#include <gygax/net/link.hpp>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>

namespace gygax::satlink {

namespace {

constexpr std::size_t kGreetingBytes = 12;
constexpr std::size_t kMaxIqBytesPerRead = std::size_t{1} << 26;
constexpr std::uint32_t kMaxSampleRate = 3'200'000;
constexpr double kU8Center = 127.5;
constexpr double kPowerFloor = 1e-30;

std::uint32_t bigEndian32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

} // namespace

std::string_view RtlTcpInfo::tunerName() const noexcept {
    switch (tuner_type) {
    case 1: return "E4000";
    case 2: return "FC0012";
    case 3: return "FC0013";
    case 4: return "FC2580";
    case 5: return "R820T";
    case 6: return "R828D";
    default: return "unknown";
    }
}

RtlTcpClient::RtlTcpClient(std::string host, int port, int timeout_ms)
    : host_(host.empty() ? "127.0.0.1" : std::move(host)), port_(port), timeout_ms_(timeout_ms) {
    if (port_ <= 0 || port_ > 65535) throw SdrError(std::format("rtl_tcp port {} is out of range", port_));
    if (timeout_ms_ <= 0) throw SdrError("rtl_tcp timeout must be positive");
    std::string error;
    link_ = net::openTcpLink(host_, static_cast<std::uint16_t>(port_), std::chrono::milliseconds(timeout_ms_), &error);
    if (!link_) throw SdrError(std::format("rtl_tcp at {}:{}: {}", host_, port_, error));
    std::uint8_t greeting[kGreetingBytes];
    readExactly(greeting, sizeof(greeting));
    if (std::memcmp(greeting, "RTL0", 4) != 0)
        throw SdrError(std::format("{}:{} is not an rtl_tcp server (no RTL0 greeting)", host_, port_));
    info_.tuner_type = bigEndian32(greeting + 4);
    info_.gain_stages = bigEndian32(greeting + 8);
}

RtlTcpClient::~RtlTcpClient() = default;
RtlTcpClient::RtlTcpClient(RtlTcpClient&&) noexcept = default;
RtlTcpClient& RtlTcpClient::operator=(RtlTcpClient&&) noexcept = default;

void RtlTcpClient::send(RtlTcpCommand command, std::uint32_t argument) {
    const std::uint8_t frame[5] = {static_cast<std::uint8_t>(command), static_cast<std::uint8_t>(argument >> 24),
                                   static_cast<std::uint8_t>(argument >> 16), static_cast<std::uint8_t>(argument >> 8),
                                   static_cast<std::uint8_t>(argument)};
    if (!link_ || link_->write(frame) != 0) throw SdrError(std::format("rtl_tcp at {}:{}: connection lost", host_, port_));
}

void RtlTcpClient::setCenterFrequency(double hz) {
    if (!(hz > 0.0) || hz > 4.0e9) throw SdrError(std::format("cannot tune rtl_tcp to {} Hz", hz));
    send(RtlTcpCommand::CenterFrequency, static_cast<std::uint32_t>(std::llround(hz)));
    center_hz_ = static_cast<double>(std::llround(hz));
}

void RtlTcpClient::setSampleRate(std::uint32_t samples_per_second) {
    if (samples_per_second == 0 || samples_per_second > kMaxSampleRate)
        throw SdrError(std::format("rtl_tcp sample rate {} is outside 1..{}", samples_per_second, kMaxSampleRate));
    send(RtlTcpCommand::SampleRate, samples_per_second);
    sample_rate_ = samples_per_second;
}

void RtlTcpClient::setAutomaticGain() {
    send(RtlTcpCommand::GainMode, 0);
}

void RtlTcpClient::setGainDb(double db) {
    if (!std::isfinite(db) || db < 0.0 || db > 60.0) throw SdrError(std::format("gain {} dB is outside 0..60", db));
    send(RtlTcpCommand::GainMode, 1);
    send(RtlTcpCommand::Gain, static_cast<std::uint32_t>(std::lround(db * 10.0)));
}

void RtlTcpClient::setFrequencyCorrection(int ppm) {
    send(RtlTcpCommand::FrequencyCorrection, static_cast<std::uint32_t>(ppm)); // two's complement on the wire
}

void RtlTcpClient::setBiasTee(bool on) {
    send(RtlTcpCommand::BiasTee, on ? 1 : 0);
}

void RtlTcpClient::readExactly(std::uint8_t* out, std::size_t size) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
    std::vector<std::uint8_t> chunk;
    while (pending_.size() < size) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) throw SdrError(std::format("rtl_tcp at {}:{} sent no data for {} ms", host_, port_, timeout_ms_));
        const int rc = link_->read(chunk, left);
        if (rc == -ETIMEDOUT) continue;
        if (rc != 0) throw SdrError(std::format("rtl_tcp at {}:{}: connection closed ({})", host_, port_, std::strerror(-rc)));
        pending_.insert(pending_.end(), chunk.begin(), chunk.end());
    }
    std::memcpy(out, pending_.data(), size);
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(size));
}

std::vector<Iq> RtlTcpClient::read(std::size_t count) {
    if (count * 2 > kMaxIqBytesPerRead) throw SdrError("rtl_tcp read is too large; read in smaller blocks");
    std::vector<std::uint8_t> bytes(count * 2);
    readExactly(bytes.data(), bytes.size());
    return iqFromU8(bytes);
}

void RtlTcpClient::discard(std::size_t count) {
    constexpr std::size_t kBlock = 65536;
    while (count > 0) {
        const auto n = std::min(count, kBlock);
        (void)read(n);
        count -= n;
    }
}

std::vector<Iq> iqFromU8(std::span<const std::uint8_t> bytes) {
    std::vector<Iq> out(bytes.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = {static_cast<float>((bytes[2 * i] - kU8Center) / kU8Center),
                  static_cast<float>((bytes[2 * i + 1] - kU8Center) / kU8Center)};
    }
    return out;
}

void fft(std::span<std::complex<double>> a) {
    const std::size_t n = a.size();
    if (n == 0 || !std::has_single_bit(n)) throw std::invalid_argument("fft size must be a power of two");
    // Bit-reversal permutation, then iterative Cooley-Tukey butterflies.
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * std::numbers::pi / static_cast<double>(len);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

std::vector<double> powerSpectrumDb(std::span<const Iq> iq, std::size_t fft_size) {
    if (fft_size < 2 || !std::has_single_bit(fft_size)) throw std::invalid_argument("fft size must be a power of two");
    const std::size_t blocks = iq.size() / fft_size;
    if (blocks == 0) throw std::invalid_argument("fewer samples than one FFT block");
    std::vector<double> window(fft_size);
    double windowPower = 0.0;
    for (std::size_t i = 0; i < fft_size; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(fft_size));
        windowPower += window[i];
    }
    std::vector<double> power(fft_size, 0.0);
    std::vector<std::complex<double>> buf(fft_size);
    for (std::size_t b = 0; b < blocks; ++b) {
        for (std::size_t i = 0; i < fft_size; ++i) buf[i] = std::complex<double>(iq[b * fft_size + i]) * window[i];
        fft(buf);
        for (std::size_t i = 0; i < fft_size; ++i) power[(i + fft_size / 2) % fft_size] += std::norm(buf[i]);
    }
    // Normalise so a full-scale complex tone on a bin centre reads 0 dB.
    const double scale = 1.0 / (static_cast<double>(blocks) * windowPower * windowPower);
    std::vector<double> db(fft_size);
    for (std::size_t i = 0; i < fft_size; ++i) db[i] = 10.0 * std::log10(std::max(power[i] * scale, kPowerFloor));
    return db;
}

SpectralPeak strongestPeak(std::span<const double> s, double sample_rate_hz) {
    if (s.size() < 3) throw std::invalid_argument("spectrum too short");
    const auto best = static_cast<std::size_t>(std::max_element(s.begin(), s.end()) - s.begin());
    // Parabolic interpolation on the dB values around the peak bin.
    double delta = 0.0;
    if (best > 0 && best + 1 < s.size()) {
        const double l = s[best - 1], c = s[best], r = s[best + 1];
        const double denom = l - 2.0 * c + r;
        if (denom < 0.0) delta = std::clamp(0.5 * (l - r) / denom, -0.5, 0.5);
    }
    std::vector<double> sorted(s.begin(), s.end());
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    const double floor = sorted[sorted.size() / 2];
    const double binHz = sample_rate_hz / static_cast<double>(s.size());
    const std::size_t dcBin = s.size() / 2; // powerSpectrumDb puts DC at the middle bin
    const double offset = (static_cast<double>(best) + delta - static_cast<double>(dcBin)) * binHz;
    return {offset, s[best], s[best] - floor};
}

DopplerMixer::DopplerMixer(double sample_rate_hz) : sample_rate_(sample_rate_hz) {
    if (!(sample_rate_hz > 0.0)) throw std::invalid_argument("sample rate must be positive");
}

void DopplerMixer::apply(std::span<Iq> iq, double start_hz, double end_hz) {
    const auto n = static_cast<double>(iq.size());
    const double f0 = start_hz / sample_rate_;                                 // turns per sample
    const double slope = n > 1 ? (end_hz - start_hz) / sample_rate_ / n : 0.0; // turns per sample per sample
    for (std::size_t k = 0; k < iq.size(); ++k) {
        const double kk = static_cast<double>(k);
        double turns = phase_ + f0 * kk + 0.5 * slope * kk * kk;
        turns -= std::floor(turns);
        const double a = -2.0 * std::numbers::pi * turns;
        iq[k] *= Iq(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
    double next = phase_ + f0 * n + 0.5 * slope * n * n;
    phase_ = next - std::floor(next);
}

} // namespace gygax::satlink
