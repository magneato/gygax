#include <gygax/neuro/events.hpp>

#include <algorithm>
#include <cmath>

namespace gygax::neuro {

namespace {

constexpr std::size_t kEvt2WordSizeBytes = 4;
constexpr std::uint32_t kEvt2TypeShift = 28;
constexpr std::uint32_t kEvt2TypeMask = 0xFU;
constexpr std::uint32_t kEvt2TimestampHighType = 0x8U;
constexpr std::uint32_t kEvt2TimestampHighMask = 0x0FFFFFFFU;
constexpr std::uint32_t kEvt2TimestampLowShift = 22;
constexpr std::uint32_t kEvt2TimestampLowMask = 0x3FU;
constexpr std::uint32_t kEvt2TimestampLowBits = 6;
constexpr std::uint32_t kEvt2CoordinateShift = 11;
constexpr std::uint32_t kEvt2CoordinateMask = 0x7FFU;
constexpr std::uint32_t kEvt2NegativeEventType = 0x0U;
constexpr std::uint32_t kEvt2PositiveEventType = 0x1U;
constexpr std::size_t kEventPolarityChannels = 2;
constexpr double kMicrosecondsPerMillisecond = 1000.0;
constexpr double kMillisecondsPerSecond = 1000.0;

}

Evt2Decoded decodeEvt2(const std::uint8_t* data, std::size_t size) {
    Evt2Decoded out;
    std::uint64_t high = 0;
    for (std::size_t at = 0; at + kEvt2WordSizeBytes <= size; at += kEvt2WordSizeBytes) {
        const std::uint32_t word = static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1]) << 8) |
                                   (static_cast<std::uint32_t>(data[at + 2]) << 16) | (static_cast<std::uint32_t>(data[at + 3]) << 24);
        const std::uint32_t type = (word >> kEvt2TypeShift) & kEvt2TypeMask;
        if (type == kEvt2TimestampHighType) {
            high = word & kEvt2TimestampHighMask;
        } else if (type == kEvt2NegativeEventType || type == kEvt2PositiveEventType) {
            Event e;
            e.timeUs = (high << kEvt2TimestampLowBits) | ((word >> kEvt2TimestampLowShift) & kEvt2TimestampLowMask);
            e.x = static_cast<std::uint16_t>((word >> kEvt2CoordinateShift) & kEvt2CoordinateMask);
            e.y = static_cast<std::uint16_t>(word & kEvt2CoordinateMask);
            e.positive = type == kEvt2PositiveEventType;
            out.events.push_back(e);
        } else {
            ++out.skippedWords;
        }
    }
    return out;
}

std::vector<float> eventFrame(const std::vector<Event>& events, std::size_t width, std::size_t height, std::uint64_t t0Us,
                              std::uint64_t t1Us) {
    std::vector<float> frame(width * height, 0.0F);
    for (const auto& e : events) {
        if (e.timeUs < t0Us || e.timeUs >= t1Us || e.x >= width || e.y >= height) continue;
        frame[e.y * width + e.x] += e.positive ? 1.0F : -1.0F;
    }
    return frame;
}

std::vector<float> timeSurface(const std::vector<Event>& events, std::size_t width, std::size_t height, std::uint64_t nowUs, double tauUs) {
    std::vector<double> last(kEventPolarityChannels * width * height, -1.0);
    for (const auto& e : events) {
        if (e.timeUs > nowUs || e.x >= width || e.y >= height) continue;
        auto& slot = last[(e.positive ? 0 : width * height) + e.y * width + e.x];
        slot = std::max(slot, static_cast<double>(e.timeUs));
    }
    std::vector<float> out(last.size(), 0.0F);
    for (std::size_t i = 0; i < last.size(); ++i) {
        if (last[i] >= 0.0) out[i] = static_cast<float>(std::exp(-(static_cast<double>(nowUs) - last[i]) / tauUs));
    }
    return out;
}

std::vector<float> voxelGrid(const std::vector<Event>& events, std::size_t width, std::size_t height, std::size_t bins, std::uint64_t t0Us,
                             std::uint64_t t1Us) {
    std::vector<float> grid(bins * width * height, 0.0F);
    if (bins == 0 || t1Us <= t0Us) return grid;
    const double span = static_cast<double>(t1Us - t0Us);
    for (const auto& e : events) {
        if (e.timeUs < t0Us || e.timeUs >= t1Us || e.x >= width || e.y >= height) continue;
        const double pos = static_cast<double>(e.timeUs - t0Us) / span * static_cast<double>(bins - 1);
        const auto lo = static_cast<std::size_t>(pos);
        const double frac = pos - static_cast<double>(lo);
        const float polarity = e.positive ? 1.0F : -1.0F;
        grid[lo * width * height + e.y * width + e.x] += polarity * static_cast<float>(1.0 - frac);
        if (lo + 1 < bins) grid[(lo + 1) * width * height + e.y * width + e.x] += polarity * static_cast<float>(frac);
    }
    return grid;
}

std::vector<Spike> eventsToSpikes(const std::vector<Event>& events, std::size_t width, std::size_t height, std::size_t downsample,
                                  std::uint64_t originUs) {
    const std::size_t ds = std::max<std::size_t>(1, downsample);
    const std::size_t w = (width + ds - 1) / ds;
    const std::size_t h = (height + ds - 1) / ds;
    std::vector<Spike> out;
    out.reserve(events.size());
    for (const auto& e : events) {
        if (e.x >= width || e.y >= height || e.timeUs < originUs) continue;
        const std::size_t idx = (e.positive ? 0 : w * h) + (e.y / ds) * w + (e.x / ds);
        out.push_back({static_cast<double>(e.timeUs - originUs) / kMicrosecondsPerMillisecond, static_cast<std::uint32_t>(idx)});
    }
    std::stable_sort(out.begin(), out.end(), [](const Spike& a, const Spike& b) { return a.timeMs < b.timeMs; });
    return out;
}

EventInjector::EventInjector(Network& network, PopulationId population, std::vector<Spike> spikes)
    : network_(network), population_(population), spikes_(std::move(spikes)) {}

std::size_t EventInjector::advanceTo(double timeMs) {
    std::size_t injected = 0;
    while (next_ < spikes_.size() && spikes_[next_].timeMs <= timeMs) {
        network_.injectSpike(population_, spikes_[next_].neuron);
        ++next_;
        ++injected;
    }
    return injected;
}

PopulationVector decodePopulationVector(const std::vector<std::uint32_t>& counts, const std::vector<double>& preferredAngles) {
    double sx = 0.0;
    double sy = 0.0;
    double total = 0.0;
    const std::size_t n = std::min(counts.size(), preferredAngles.size());
    for (std::size_t i = 0; i < n; ++i) {
        sx += counts[i] * std::cos(preferredAngles[i]);
        sy += counts[i] * std::sin(preferredAngles[i]);
        total += counts[i];
    }
    if (total <= 0.0) return {};
    return {std::atan2(sy, sx), std::hypot(sx, sy) / total};
}

RateDecoder::RateDecoder(std::size_t outputs, double tauMs, double maxRateHz)
    : value_(outputs, 0.0), tauMs_(tauMs), maxRateHz_(maxRateHz) {}

const std::vector<double>& RateDecoder::update(const std::vector<std::uint32_t>& counts, double windowMs) {
    const double alpha = tauMs_ <= 0.0 ? 1.0 : 1.0 - std::exp(-windowMs / tauMs_);
    for (std::size_t i = 0; i < value_.size(); ++i) {
        const double rate = i < counts.size() ? counts[i] * kMillisecondsPerSecond / windowMs : 0.0;
        const double norm = std::clamp(rate / maxRateHz_, 0.0, 1.0);
        value_[i] += alpha * (norm - value_[i]);
    }
    return value_;
}

void RateDecoder::reset() {
    std::fill(value_.begin(), value_.end(), 0.0);
}

bool PdmModulator::next(double duty) {
    accumulator_ += std::clamp(duty, 0.0, 1.0);
    if (accumulator_ >= 1.0) {
        accumulator_ -= 1.0;
        return true;
    }
    return false;
}

}
