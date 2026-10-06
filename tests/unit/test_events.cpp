#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include <gygax/neuro/events.hpp>

using namespace gygax::neuro;

namespace {

static_assert(alignof(Event) == 16);
static_assert(alignof(Spike) == 16);
static_assert(alignof(PopulationVector) == 16);
static_assert(sizeof(Event) == 16);
static_assert(sizeof(Spike) == 16);
static_assert(sizeof(PopulationVector) == 16);

std::uint32_t cd(bool on, std::uint32_t tLow, std::uint32_t x, std::uint32_t y) {
    return ((on ? 1U : 0U) << 28) | (tLow << 22) | (x << 11) | y;
}

void push(std::vector<std::uint8_t>& out, std::uint32_t w) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
}

}

TEST(Evt2, DecodesTimestampsCoordinatesAndPolarity) {
    std::vector<std::uint8_t> bytes;
    push(bytes, (0x8U << 28) | 2U);
    push(bytes, cd(true, 5, 100, 200));
    push(bytes, cd(false, 63, 1, 2));
    push(bytes, (0xAU << 28) | 7U);
    push(bytes, (0x8U << 28) | 3U);
    push(bytes, cd(true, 0, 639, 479));
    bytes.push_back(0xFF);
    const auto d = decodeEvt2(bytes.data(), bytes.size());
    ASSERT_EQ(d.events.size(), 3U);
    EXPECT_EQ(d.skippedWords, 1U);
    EXPECT_EQ(d.events[0].timeUs, (2U << 6) | 5U);
    EXPECT_EQ(d.events[0].x, 100);
    EXPECT_EQ(d.events[0].y, 200);
    EXPECT_TRUE(d.events[0].positive);
    EXPECT_FALSE(d.events[1].positive);
    EXPECT_EQ(d.events[1].timeUs, (2U << 6) | 63U);
    EXPECT_EQ(d.events[2].timeUs, 3U << 6);
    EXPECT_EQ(d.events[2].x, 639);
}

TEST(EventRepresentations, FrameSurfaceAndVoxelGrid) {
    const std::vector<Event> ev{{100, 1, 1, true}, {200, 1, 1, true}, {300, 2, 0, false}, {5000, 0, 0, true}};
    const auto frame = eventFrame(ev, 4, 2, 0, 1000);
    EXPECT_EQ(frame[1 * 4 + 1], 2.0F);
    EXPECT_EQ(frame[0 * 4 + 2], -1.0F);
    EXPECT_EQ(frame[0], 0.0F);

    const auto surface = timeSurface(ev, 4, 2, 300, 100.0);
    EXPECT_NEAR(surface[1 * 4 + 1], std::exp(-1.0), 1e-6);
    EXPECT_NEAR(surface[8 + 0 * 4 + 2], 1.0, 1e-6);
    EXPECT_EQ(surface[0], 0.0F);

    const auto grid = voxelGrid(ev, 4, 2, 3, 0, 1000);
    float total = 0.0F;
    for (float v : grid) total += v;
    EXPECT_NEAR(total, 2.0F - 1.0F, 1e-5);
    EXPECT_EQ(grid.size(), 3U * 8U);
}

TEST(EventRepresentations, VectorStorageHonorsExplicitAlignment) {
    const std::vector<Event> events{{100, 1, 1, true}, {200, 2, 2, false}};
    const std::vector<Spike> spikes{{1.0, 2}, {2.0, 3}};
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(events.data()) % alignof(Event), 0U);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(spikes.data()) % alignof(Spike), 0U);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(events.data() + 1) % alignof(Event), 0U);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(spikes.data() + 1) % alignof(Spike), 0U);
}

TEST(EventSpikes, DownsampleAndInjectIntoANetwork) {
    const std::vector<Event> ev{{2000, 3, 1, true}, {1000, 0, 0, false}, {3000, 1, 1, true}};
    const auto spikes = eventsToSpikes(ev, 4, 4, 2, 0);
    ASSERT_EQ(spikes.size(), 3U);
    EXPECT_LE(spikes[0].timeMs, spikes[1].timeMs);
    EXPECT_EQ(spikes[0].neuron, 4U + 0U);
    EXPECT_EQ(spikes[1].neuron, 1U);
    EXPECT_EQ(spikes[2].neuron, 0U);

    Network net(0.1, 1);
    const auto pop = net.addSpikeSource("dvs", 8);
    EventInjector injector(net, pop, spikes);
    EXPECT_EQ(injector.advanceTo(1.5), 1U);
    net.step();
    EXPECT_EQ(injector.advanceTo(10.0), 2U);
    EXPECT_EQ(injector.remaining(), 0U);
    net.step();
    EXPECT_EQ(net.spikeCount(pop), 3U);
}

TEST(Decoders, PopulationVectorRecoversTheSteeringAngle) {
    std::vector<double> angles;
    std::vector<std::uint32_t> counts;
    for (int i = 0; i < 16; ++i) {
        const double a = 2.0 * M_PI * i / 16;
        angles.push_back(a);
        counts.push_back(static_cast<std::uint32_t>(std::lround(50.0 * std::max(0.0, std::cos(a - 1.0)))));
    }
    const auto pv = decodePopulationVector(counts, angles);
    EXPECT_NEAR(pv.angle, 1.0, 0.05);
    EXPECT_GT(pv.magnitude, 0.5);
    EXPECT_EQ(decodePopulationVector({0, 0}, {0.0, 1.0}).magnitude, 0.0);
}

TEST(Decoders, RateDecoderSmoothsAndSaturates) {
    RateDecoder dec(2, 50.0, 100.0);
    for (int i = 0; i < 100; ++i) dec.update({1, 5}, 10.0);
    EXPECT_NEAR(dec.value()[0], 1.0, 1e-3);
    EXPECT_NEAR(dec.value()[1], 1.0, 1e-3);
    const auto first = RateDecoder(1, 100.0, 100.0).update({1}, 10.0)[0];
    EXPECT_GT(first, 0.0);
    EXPECT_LT(first, 1.0);
    dec.reset();
    EXPECT_EQ(dec.value()[0], 0.0);
}

TEST(Decoders, PdmMatchesTheRequestedDuty) {
    PdmModulator pdm;
    int on = 0;
    for (int i = 0; i < 1000; ++i) on += pdm.next(0.3) ? 1 : 0;
    EXPECT_NEAR(on, 300, 1);
    PdmModulator full;
    for (int i = 0; i < 10; ++i) EXPECT_TRUE(full.next(1.5));
    PdmModulator zero;
    for (int i = 0; i < 10; ++i) EXPECT_FALSE(zero.next(-1.0));
}
