// Accessibility: colour vision correction and audio cues.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <glyder/audio.hpp>
#include <glyder/colour.hpp>

using namespace glyder;

namespace {

float distance(Vec3 a, Vec3 b) {
    return length(a - b);
}

// Pairs a viewer with each deficiency confuses: red/green for protan and deutan, blue/green and
// yellow/white-ish for tritan. Linear RGB.
struct Pair {
    Vec3 a, b;
};

} // namespace

TEST(GlyderColour, SimulationAndCorrectionAreIdentityForTypicalVision) {
    const Vec3 c{0.2f, 0.5f, 0.8f};
    EXPECT_LT(distance(colour::simulate(colour::Vision::Typical).apply(c), c), 1e-6f);
    EXPECT_LT(distance(colour::correct(colour::Vision::Typical).apply(c), c), 1e-6f);
    EXPECT_LT(distance(colour::display(colour::Vision::Typical, true, true).apply(c), c), 1e-6f);
}

TEST(GlyderColour, SimulationKeepsWhiteAndLosesTheConfusedDifference) {
    for (auto v : {colour::Vision::Protan, colour::Vision::Deutan, colour::Vision::Tritan}) {
        SCOPED_TRACE(std::string(colour::name(v)));
        // Machado's matrices keep white white.
        EXPECT_LT(distance(colour::simulate(v).apply({1, 1, 1}), {1, 1, 1}), 0.01f);
    }
    // A red-green pair is far closer for a protan or deutan viewer than for a typical one.
    const Vec3 red{0.8f, 0.2f, 0.1f}, green{0.2f, 0.55f, 0.1f};
    for (auto v : {colour::Vision::Protan, colour::Vision::Deutan}) {
        const auto s = colour::simulate(v);
        EXPECT_LT(distance(s.apply(red), s.apply(green)), 0.6f * distance(red, green));
    }
}

TEST(GlyderColour, CorrectionRestoresDifferencesTheViewerLost) {
    // For each deficiency, a pair of colours it confuses: after correction, the viewer (simulated)
    // tells them apart better than without.
    const std::vector<std::pair<colour::Vision, Pair>> cases{
        {colour::Vision::Protan, {{0.8f, 0.2f, 0.1f}, {0.2f, 0.55f, 0.1f}}},
        {colour::Vision::Deutan, {{0.8f, 0.2f, 0.1f}, {0.2f, 0.55f, 0.1f}}},
        {colour::Vision::Tritan, {{0.1f, 0.3f, 0.9f}, {0.1f, 0.6f, 0.35f}}},
    };
    for (const auto& [v, p] : cases) {
        SCOPED_TRACE(std::string(colour::name(v)));
        const auto seen = colour::simulate(v);
        const auto fixed = colour::display(v, true, true);
        const float before = distance(seen.apply(p.a), seen.apply(p.b));
        const float after = distance(fixed.apply(p.a), fixed.apply(p.b));
        EXPECT_GT(after, before * 1.15f) << "before " << before << " after " << after;
    }
}

TEST(GlyderColour, TokensRoundTripAndTheMenuReachesEveryMode) {
    colour::Vision v = colour::Vision::Typical;
    EXPECT_EQ(colour::next(v), colour::Vision::Tritan); // tritan first
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(colour::fromToken(colour::token(v)), v);
        v = colour::next(v);
    }
    EXPECT_EQ(v, colour::Vision::Typical);
}

TEST(GlyderAudio, EachStageHasItsOwnWaveform) {
    // A threat's attack is a square wave: only the two levels of a square, scaled by the rising
    // envelope, never anything in between at a fixed level.
    const auto& e = audio::envelope(audio::Cue::Threat);
    EXPECT_EQ(e.attack.wave, audio::Wave::Square);
    EXPECT_EQ(e.release.wave, audio::Wave::Sine);
    EXPECT_NE(audio::envelope(audio::Cue::Air).attack.wave, e.attack.wave);
    std::uint32_t n = 1;
    EXPECT_FLOAT_EQ(audio::wave(audio::Wave::Square, 0.25f, n), 1.0f);
    EXPECT_FLOAT_EQ(audio::wave(audio::Wave::Square, 0.75f, n), -1.0f);
    EXPECT_NEAR(audio::wave(audio::Wave::Sine, 0.25f, n), 1.0f, 1e-6f);
    EXPECT_NEAR(audio::wave(audio::Wave::Triangle, 0.5f, n), 1.0f, 1e-6f);
    EXPECT_NEAR(audio::wave(audio::Wave::Saw, 0.0f, n), -1.0f, 1e-6f);
}

TEST(GlyderAudio, ASourceOnTheRightIsLouderOnTheRight) {
    audio::Mixer m(48000);
    audio::Source s{1, audio::Cue::Air, {1, 0, 0}, 50, 1000};
    m.update(std::span(&s, 1));
    std::vector<float> out(48000 * 2);
    m.render(out);
    double l = 0, r = 0;
    for (std::size_t i = 0; i < out.size(); i += 2) {
        l += out[i] * out[i];
        r += out[i + 1] * out[i + 1];
        ASSERT_TRUE(std::isfinite(out[i]) && std::abs(out[i]) <= 1.0f);
    }
    EXPECT_GT(r, 0.0);
    EXPECT_GT(r, 20 * l);
}

TEST(GlyderAudio, IncomingThreatsRepeatFasterAndNeverGoQuiet) {
    audio::Source far{1, audio::Cue::Threat, {0, 0, -1}, 900, 1000, 300, 3.0f};
    audio::Source near = far;
    near.distance = 100;
    near.timeToGo = 0.3f;
    const auto vf = audio::voicing(far), vn = audio::voicing(near);
    EXPECT_LT(vn.period, vf.period);
    EXPECT_GE(vf.gain, 0.35f); // far, but inbound: still heard
    // Above sounds higher than below.
    audio::Source up = far, down = far;
    up.direction = {0, 1, 0};
    down.direction = {0, -1, 0};
    EXPECT_GT(audio::voicing(up).hz, audio::voicing(down).hz * 3.5f);
}

TEST(GlyderAudio, RenderingIsDeterministicAndWritesAValidWav) {
    auto run = [] {
        audio::Mixer m(48000);
        std::vector<audio::Source> s{{1, audio::Cue::Threat, {0, 0, -1}, 200, 1000, 300, 0.7f},
                                     {2, audio::Cue::Ground, {-1, 0, 0}, 50, 1000},
                                     {3, audio::Cue::Friend, {0, 0.6f, -0.8f}, 20, 1000}};
        m.update(s);
        m.note(audio::Cue::Event, 784, 0.6f, 0, 1175);
        std::vector<float> all;
        for (int f = 0; f < 30; ++f) {
            std::vector<float> block(1600 * 2);
            m.render(block);
            all.insert(all.end(), block.begin(), block.end());
        }
        return all;
    };
    const auto a = run(), b = run();
    ASSERT_EQ(a, b);
    const auto w = audio::wav(a, 48000);
    ASSERT_EQ(w.size(), 44 + a.size() * 2);
    EXPECT_EQ(std::string(w.begin(), w.begin() + 4), "RIFF");
    EXPECT_EQ(std::string(w.begin() + 8, w.begin() + 12), "WAVE");
}
