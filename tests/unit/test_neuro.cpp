#include <gtest/gtest.h>

#include <cmath>

#include <gygax/neuro/network.hpp>

using namespace gygax::neuro;

TEST(Rng, IsDeterministicAndRoughlyUniform) {
    Rng a(42);
    Rng b(42);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(a.next(), b.next());
    Rng c(43);
    EXPECT_NE(Rng(42).next(), c.next());
    Rng u(1);
    double sum = 0.0;
    constexpr int n = 200000;
    for (int i = 0; i < n; ++i) {
        const double x = u.uniform();
        ASSERT_GE(x, 0.0);
        ASSERT_LT(x, 1.0);
        sum += x;
    }
    EXPECT_NEAR(sum / n, 0.5, 0.005);
}

TEST(Rng, NormalHasUnitMoments) {
    Rng r(9);
    double sum = 0.0;
    double sq = 0.0;
    constexpr int n = 200000;
    for (int i = 0; i < n; ++i) {
        const double x = r.normal();
        sum += x;
        sq += x * x;
    }
    EXPECT_NEAR(sum / n, 0.0, 0.01);
    EXPECT_NEAR(sq / n, 1.0, 0.02);
}

TEST(Lif, SubthresholdDriveNeverSpikesAndSettlesAtVInf) {
    Network net(0.1, 1);
    LifParams p;
    const auto pop = net.addLif("n", 1, p);
    net.setBias(pop, 10.0);
    net.run(500);
    EXPECT_EQ(net.spikeCount(pop), 0U);
    EXPECT_NEAR(net.membranePotentials(pop)[0], p.vRest + p.resistance * 10.0, 1e-3);
}

class LifRate : public ::testing::TestWithParam<double> {};

TEST_P(LifRate, MatchesTheAnalyticFICurve) {
    const double current = GetParam();
    Network net(0.05, 1);
    LifParams p;
    const auto pop = net.addLif("n", 1, p);
    net.setBias(pop, current);
    net.run(4000);
    const double expected = lifSteadyStateRateHz(p, current);
    EXPECT_NEAR(net.meanRateHz(pop), expected, 0.05 * expected + 0.5);
}

INSTANTIATE_TEST_SUITE_P(Currents, LifRate, ::testing::Values(16.0, 18.0, 20.0, 25.0, 30.0, 40.0));

TEST(Lif, RefractoryPeriodCapsTheRate) {
    Network net(0.05, 1);
    LifParams p;
    p.tauRef = 5.0;
    const auto pop = net.addLif("n", 1, p);
    net.setBias(pop, 500.0);
    net.run(1000);
    EXPECT_LE(net.meanRateHz(pop), 1000.0 / p.tauRef + 1.0);
    EXPECT_GT(net.meanRateHz(pop), 100.0);
}

TEST(Izhikevich, RegularSpikingFiresUnderSteadyDrive) {
    Network net(0.25, 1);
    const auto pop = net.addIzhikevich("rs", 1);
    net.setBias(pop, 10.0);
    net.run(1000);
    EXPECT_GT(net.spikeCount(pop), 5U);
    EXPECT_LT(net.spikeCount(pop), 100U);
    Network quiet(0.25, 1);
    const auto q = quiet.addIzhikevich("rs", 1);
    quiet.run(1000);
    EXPECT_EQ(quiet.spikeCount(q), 0U);
}

TEST(Poisson, RateIsAccurate) {
    Network net(0.1, 5);
    const auto pop = net.addPoisson("p", 200, 25.0);
    net.run(4000);
    EXPECT_NEAR(net.meanRateHz(pop), 25.0, 1.0);
}

TEST(Poisson, PerNeuronRatesAndValidation) {
    Network net(0.1, 5);
    const auto pop = net.addPoisson("p", 2, 0.0);
    net.setRates(pop, {100.0, 0.0});
    net.run(2000);
    const auto counts = net.spikeCounts(pop);
    EXPECT_GT(counts[0], 100U);
    EXPECT_EQ(counts[1], 0U);
    EXPECT_THROW(net.setRates(pop, {1.0}), std::invalid_argument);
    EXPECT_THROW(net.setRates(pop, {-1.0, 0.0}), std::invalid_argument);
}

TEST(Network, DelaysAreRespected) {
    Network net(0.1, 1);
    const auto src = net.addSpikeSource("src", 1);
    const auto dst = net.addLif("dst", 1);
    net.connectExplicit(src, dst, {0}, {0}, {150.0F}, 5.0);
    net.injectSpike(src, 0);
    net.run(4.0);
    EXPECT_EQ(net.spikeCount(dst), 0U);
    net.run(15.0);
    ASSERT_GE(net.spikeCount(dst), 1U);
    EXPECT_GE(net.spikes(dst).front().timeMs, 5.0);
}

TEST(Network, InhibitionSuppressesFiring) {
    auto rate = [](float inhibition) {
        Network net(0.1, 3);
        const auto ex = net.addPoisson("ex", 20, 100.0);
        const auto in = net.addPoisson("in", 20, 100.0);
        const auto out = net.addLif("out", 1);
        ConnectionSpec e;
        e.kind = Connectivity::AllToAll;
        e.weightMean = 3.0;
        net.connect(ex, out, e);
        ConnectionSpec i;
        i.kind = Connectivity::AllToAll;
        i.weightMean = static_cast<double>(inhibition);
        net.connect(in, out, i);
        net.run(2000);
        return net.meanRateHz(out);
    };
    const double baseline = rate(0.0F);
    ASSERT_GT(baseline, 5.0);
    EXPECT_LT(rate(-3.0F), baseline * 0.5);
}

TEST(Network, ConnectivityKinds) {
    Network net(0.1, 7);
    const auto a = net.addLif("a", 10);
    const auto b = net.addLif("b", 10);
    ConnectionSpec spec;
    spec.kind = Connectivity::AllToAll;
    EXPECT_EQ(net.synapseCount(net.connect(a, b, spec)), 100U);
    EXPECT_EQ(net.synapseCount(net.connect(a, a, spec)), 90U);
    spec.allowSelf = true;
    EXPECT_EQ(net.synapseCount(net.connect(a, a, spec)), 100U);
    spec.kind = Connectivity::OneToOne;
    EXPECT_EQ(net.synapseCount(net.connect(a, b, spec)), 10U);
    spec.kind = Connectivity::FixedProbability;
    spec.probability = 0.5;
    const auto n = net.synapseCount(net.connect(a, b, spec));
    EXPECT_GT(n, 20U);
    EXPECT_LT(n, 80U);
    spec.probability = 0.0;
    EXPECT_EQ(net.synapseCount(net.connect(a, b, spec)), 0U);
}

TEST(Network, ValidatesArguments) {
    Network net(0.1, 1);
    const auto lif = net.addLif("a", 4);
    const auto poisson = net.addPoisson("p", 4, 10.0);
    const auto bigger = net.addLif("b", 5);
    EXPECT_THROW(net.addLif("a", 1), std::invalid_argument);
    EXPECT_THROW(net.addLif("z", 0), std::invalid_argument);
    EXPECT_THROW(net.connect(lif, poisson, {}), std::invalid_argument);
    ConnectionSpec one;
    one.kind = Connectivity::OneToOne;
    EXPECT_THROW(net.connect(lif, bigger, one), std::invalid_argument);
    ConnectionSpec bad;
    bad.probability = 1.5;
    EXPECT_THROW(net.connect(lif, bigger, bad), std::invalid_argument);
    EXPECT_THROW(net.connect(99, lif, {}), std::out_of_range);
    EXPECT_THROW(net.setDrive(lif, {1.0}), std::invalid_argument);
    EXPECT_THROW(net.injectSpike(lif, 0), std::invalid_argument);
    EXPECT_THROW(net.run(-1.0), std::invalid_argument);
    EXPECT_THROW(Network(0.0, 1), std::invalid_argument);
    EXPECT_THROW(net.connectExplicit(lif, bigger, {0}, {9}, {1.0F}, 1.0), std::out_of_range);
    EXPECT_THROW(net.connectExplicit(lif, bigger, {0}, {0, 1}, {1.0F}, 1.0), std::invalid_argument);
}

TEST(Network, IdenticalSeedsGiveIdenticalRasters) {
    auto run = [](std::uint64_t seed) {
        Network net(0.1, seed);
        const auto in = net.addPoisson("in", 30, 50.0);
        const auto out = net.addLif("out", 30);
        ConnectionSpec spec;
        spec.probability = 0.3;
        spec.weightMean = 9.0;
        spec.weightStd = 2.0;
        net.connect(in, out, spec);
        net.run(300);
        return net.spikes(out);
    };
    const auto a = run(11);
    const auto b = run(11);
    const auto c = run(12);
    ASSERT_GT(a.size(), 0U);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].neuron, b[i].neuron);
        EXPECT_DOUBLE_EQ(a[i].timeMs, b[i].timeMs);
    }
    EXPECT_NE(a.size(), c.size());
}

TEST(Network, ResetRestoresStateAndWeights) {
    Network net(0.1, 3);
    const auto pre = net.addSpikeSource("pre", 1);
    const auto post = net.addLif("post", 1);
    StdpParams stdp;
    stdp.aPlus = 0.5;
    stdp.aMinus = 0.5;
    stdp.wMax = 400.0;
    const auto proj = net.connectExplicit(pre, post, {0}, {0}, {120.0F}, 1.0, stdp);
    for (int i = 0; i < 5; ++i) {
        net.injectSpike(pre, 0);
        net.run(30);
    }
    ASSERT_GT(net.meanWeight(proj), 120.0);
    const auto first = net.spikes(post);
    net.reset();
    EXPECT_DOUBLE_EQ(net.meanWeight(proj), 120.0);
    EXPECT_DOUBLE_EQ(net.timeMs(), 0.0);
    EXPECT_EQ(net.spikeCount(post), 0U);
    for (int i = 0; i < 5; ++i) {
        net.injectSpike(pre, 0);
        net.run(30);
    }
    ASSERT_EQ(net.spikes(post).size(), first.size());
    for (std::size_t i = 0; i < first.size(); ++i) EXPECT_DOUBLE_EQ(net.spikes(post)[i].timeMs, first[i].timeMs);
}

TEST(Stdp, CausalPairingPotentiatesAndAnticausalDepresses) {
    StdpParams stdp;
    stdp.aPlus = 0.5;
    stdp.aMinus = 0.5;
    stdp.wMin = 0.0;
    stdp.wMax = 400.0;
    {
        Network net(0.1, 3);
        const auto pre = net.addSpikeSource("pre", 1);
        const auto post = net.addLif("post", 1);
        const auto proj = net.connectExplicit(pre, post, {0}, {0}, {120.0F}, 1.0, stdp);
        for (int i = 0; i < 10; ++i) {
            net.injectSpike(pre, 0);
            net.run(30);
        }
        EXPECT_EQ(net.spikeCount(post), 10U);
        EXPECT_GT(net.meanWeight(proj), 120.0);
    }
    {
        Network net(0.1, 3);
        const auto trigger = net.addSpikeSource("trigger", 1);
        const auto pre = net.addSpikeSource("pre", 1);
        const auto post = net.addLif("post", 1);
        net.connectExplicit(trigger, post, {0}, {0}, {120.0F}, 1.0);
        const auto proj = net.connectExplicit(pre, post, {0}, {0}, {2.0F}, 1.0, stdp);
        for (int i = 0; i < 10; ++i) {
            net.injectSpike(trigger, 0);
            net.run(6);
            net.injectSpike(pre, 0);
            net.run(30);
        }
        EXPECT_LT(net.meanWeight(proj), 2.0);
    }
}

TEST(Stdp, WeightsStayWithinBounds) {
    Network net(0.1, 3);
    const auto pre = net.addPoisson("pre", 20, 80.0);
    const auto post = net.addLif("post", 5);
    StdpParams stdp;
    stdp.aPlus = 0.5;
    stdp.aMinus = 0.1;
    stdp.wMin = 1.0;
    stdp.wMax = 20.0;
    ConnectionSpec spec;
    spec.kind = Connectivity::AllToAll;
    spec.weightMean = 10.0;
    spec.stdp = stdp;
    const auto proj = net.connect(pre, post, spec);
    net.run(3000);
    for (const float w : net.weights(proj)) {
        EXPECT_GE(w, 1.0F);
        EXPECT_LE(w, 20.0F);
    }
    net.setPlasticity(false);
    const auto frozen = net.weights(proj);
    net.run(500);
    EXPECT_EQ(net.weights(proj), frozen);
}

TEST(Network, VoltageRecordingAndClearSpikes) {
    Network net(0.1, 1);
    const auto pop = net.addLif("n", 1);
    net.setRecordVoltage(pop, true);
    net.setBias(pop, 20.0);
    net.run(100);
    EXPECT_EQ(net.voltageTrace(pop).size(), 1000U);
    EXPECT_GT(net.spikeCount(pop), 0U);
    net.clearSpikes();
    EXPECT_EQ(net.spikeCount(pop), 0U);
    EXPECT_TRUE(net.spikes(pop).empty());
}

TEST(Encoding, RateEncodeClampsAndArgmaxPicksTheWinner) {
    const auto rates = rateEncode({-1.0, 0.5, 2.0}, 100.0);
    EXPECT_EQ(rates, (std::vector<double>{0.0, 50.0, 100.0}));
    EXPECT_EQ(argmaxCounts({1, 9, 3}), 1U);
    EXPECT_EQ(argmaxCounts({}), 0U);
}

TEST(Spec, RunsFromJsonAndReportsSpikes) {
    const auto spec = *gygax::json::parse(R"({
      "dt": 0.1, "seed": 2, "run_ms": 500, "raster_limit": 5,
      "populations": [
        {"name": "in", "type": "poisson", "n": 50, "rate": 60},
        {"name": "out", "type": "lif", "n": 20, "record_voltage": true}
      ],
      "projections": [
        {"pre": "in", "post": "out", "connect": {"type": "fixed_prob", "p": 0.4}, "weight": {"mean": 8.0, "std": 1.0}, "delay_ms": 2.0,
         "stdp": {"a_plus": 0.02, "a_minus": 0.021, "w_max": 20}}
      ]})");
    std::string err;
    auto out = simulateSpec(spec, &err);
    ASSERT_FALSE(out.isNull()) << err;
    const auto& results = out.find("results")->asArray();
    ASSERT_EQ(results.size(), 2U);
    EXPECT_GT(results[0].getInt("spike_count"), 0);
    EXPECT_GT(results[1].getInt("spike_count"), 0);
    EXPECT_LE(results[1].find("raster")->asArray().size(), 5U);
    EXPECT_TRUE(out.find("projections")->asArray()[0].getBool("plastic"));
}

TEST(Spec, RejectsBadInputWithUsefulErrors) {
    auto expectError = [](const char* text, const char* needle) {
        std::string err;
        auto out = simulateSpec(*gygax::json::parse(text), &err);
        EXPECT_TRUE(out.isNull()) << text;
        EXPECT_NE(err.find(needle), std::string::npos) << err;
    };
    expectError(R"([])", "object");
    expectError(R"({})", "populations");
    expectError(R"({"populations":[{"name":"a","type":"weird","n":1}]})", "unknown population type");
    expectError(R"({"populations":[{"name":"a","type":"lif","n":0}]})", "at least one");
    expectError(R"({"populations":[{"name":"a","type":"lif","n":1}],"projections":[{"pre":"a","post":"nope"}]})", "unknown population");
    expectError(R"({"populations":[{"name":"a","type":"lif","n":300000}]})", "too large");
    expectError(R"({"run_ms":-5,"populations":[{"name":"a","type":"lif","n":1}]})", "run_ms");
    expectError(R"({"dt":0,"populations":[{"name":"a","type":"lif","n":1}]})", "dt");
}
