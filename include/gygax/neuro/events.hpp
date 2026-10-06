#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <gygax/neuro/network.hpp>

namespace gygax::neuro {

struct alignas(16) Event {
    std::uint64_t timeUs = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    bool positive = true;
};

struct Evt2Decoded {
    std::vector<Event> events;
    std::size_t skippedWords = 0;
};

Evt2Decoded decodeEvt2(const std::uint8_t* data, std::size_t size);

std::vector<float> eventFrame(const std::vector<Event>& events, std::size_t width, std::size_t height, std::uint64_t t0Us,
                              std::uint64_t t1Us);
std::vector<float> timeSurface(const std::vector<Event>& events, std::size_t width, std::size_t height, std::uint64_t nowUs, double tauUs);
std::vector<float> voxelGrid(const std::vector<Event>& events, std::size_t width, std::size_t height, std::size_t bins, std::uint64_t t0Us,
                             std::uint64_t t1Us);

std::vector<Spike> eventsToSpikes(const std::vector<Event>& events, std::size_t width, std::size_t height, std::size_t downsample,
                                  std::uint64_t originUs);

class EventInjector {
public:
    EventInjector(Network& network, PopulationId population, std::vector<Spike> spikes);

    std::size_t advanceTo(double timeMs);
    [[nodiscard]] std::size_t remaining() const { return spikes_.size() - next_; }

private:
    Network& network_;
    PopulationId population_;
    std::vector<Spike> spikes_;
    std::size_t next_ = 0;
};

struct alignas(16) PopulationVector {
    double angle = 0.0;
    double magnitude = 0.0;
};

PopulationVector decodePopulationVector(const std::vector<std::uint32_t>& counts, const std::vector<double>& preferredAngles);

class RateDecoder {
public:
    RateDecoder(std::size_t outputs, double tauMs, double maxRateHz);

    const std::vector<double>& update(const std::vector<std::uint32_t>& counts, double windowMs);
    [[nodiscard]] const std::vector<double>& value() const { return value_; }
    void reset();

private:
    std::vector<double> value_;
    double tauMs_;
    double maxRateHz_;
};

class PdmModulator {
public:
    bool next(double duty);
    void reset() { accumulator_ = 0.0; }

private:
    double accumulator_ = 0.0;
};

}
