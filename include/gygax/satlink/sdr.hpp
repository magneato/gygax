#pragma once

// SatLink SDR: receive a satellite with a software-defined radio.
//
//   RtlTcpClient    the rtl_tcp network protocol (RTL-SDR dongles, and the many servers and SDR
//                   programs that speak it): tune, set the sample rate and gain, stream IQ
//   powerSpectrumDb an averaged, Hann-windowed FFT power spectrum, centred on DC
//   strongestPeak   the strongest bin, refined between bins by a parabola
//   DopplerMixer    a phase-continuous mixer that removes a changing frequency offset (the
//                   satellite's Doppler) from IQ, so a moving carrier lands on a fixed one
//
// Tuning an SDR follows the radio rule in satlink.hpp: centre on carrier + Doppler shift.

#include <complex>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gygax::net {
class ByteLink;
}

namespace gygax::satlink {

inline constexpr int kRtlTcpDefaultPort = 1234;

using Iq = std::complex<float>;

class SdrError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What an rtl_tcp server reports when a client connects.
struct RtlTcpInfo {
    std::uint32_t tuner_type{0}; // rtl_tcp's numbering: 1 E4000, 2 FC0012, 3 FC0013, 4 FC2580, 5 R820T, 6 R828D
    std::uint32_t gain_stages{0};

    [[nodiscard]] std::string_view tunerName() const noexcept;
};

// The 5-byte rtl_tcp commands: one opcode byte and a big-endian 32-bit argument.
enum class RtlTcpCommand : std::uint8_t {
    CenterFrequency = 0x01,
    SampleRate = 0x02,
    GainMode = 0x03,            // 0 automatic, 1 manual
    Gain = 0x04,                // tenths of a dB
    FrequencyCorrection = 0x05, // ppm
    AgcMode = 0x08,
    DirectSampling = 0x09,
    OffsetTuning = 0x0a,
    BiasTee = 0x0e,
};

class RtlTcpClient {
public:
    // Connects and reads the 12-byte "RTL0" greeting; throws SdrError otherwise.
    explicit RtlTcpClient(std::string host = "127.0.0.1", int port = kRtlTcpDefaultPort, int timeout_ms = 2000);
    ~RtlTcpClient();
    RtlTcpClient(RtlTcpClient&&) noexcept;
    RtlTcpClient& operator=(RtlTcpClient&&) noexcept;

    [[nodiscard]] const RtlTcpInfo& info() const noexcept { return info_; }

    void setCenterFrequency(double hz);
    void setSampleRate(std::uint32_t samples_per_second);
    void setAutomaticGain();   // tuner AGC
    void setGainDb(double db); // manual gain, nearest step the tuner has
    void setFrequencyCorrection(int ppm);
    void setBiasTee(bool on);
    void send(RtlTcpCommand command, std::uint32_t argument);

    [[nodiscard]] double centerFrequency() const noexcept { return center_hz_; }
    [[nodiscard]] std::uint32_t sampleRate() const noexcept { return sample_rate_; }

    // The next `count` IQ samples, scaled to [-1, 1]. rtl_tcp streams continuously, so samples
    // queued before a retune arrive first: call discard() after tuning for a clean capture.
    [[nodiscard]] std::vector<Iq> read(std::size_t count);
    void discard(std::size_t count);

private:
    void readExactly(std::uint8_t* out, std::size_t size);

    std::string host_;
    int port_;
    int timeout_ms_;
    std::shared_ptr<net::ByteLink> link_;
    std::vector<std::uint8_t> pending_;
    RtlTcpInfo info_;
    double center_hz_{0.0};
    std::uint32_t sample_rate_{0};
};

// Unsigned 8-bit interleaved IQ (rtl_tcp, rtl_sdr files) to complex samples in [-1, 1].
[[nodiscard]] std::vector<Iq> iqFromU8(std::span<const std::uint8_t> bytes);

// In-place radix-2 FFT; the size must be a power of two.
void fft(std::span<std::complex<double>> data);

// Power in dB per bin (relative to full scale), averaged over every whole `fft_size` block in
// `iq`, with bin 0 at -fs/2 and bin fft_size/2 at DC.
[[nodiscard]] std::vector<double> powerSpectrumDb(std::span<const Iq> iq, std::size_t fft_size);

struct SpectralPeak {
    double offset_hz{0.0}; // from the centre frequency
    double power_db{0.0};
    double snr_db{0.0}; // above the median bin, a robust noise floor
};

[[nodiscard]] SpectralPeak strongestPeak(std::span<const double> spectrum_db, double sample_rate_hz);

// Removes a frequency offset that changes linearly across each block (a satellite's Doppler
// over a fraction of a second), keeping phase continuous between blocks.
class DopplerMixer {
public:
    explicit DopplerMixer(double sample_rate_hz);

    // Multiply by exp(-i 2pi f(t) t) with f moving from start_hz to end_hz over the block.
    void apply(std::span<Iq> iq, double start_hz, double end_hz);
    void apply(std::span<Iq> iq, double offset_hz) { apply(iq, offset_hz, offset_hz); }

private:
    double sample_rate_;
    double phase_{0.0}; // turns, kept in [0, 1)
};

} // namespace gygax::satlink
