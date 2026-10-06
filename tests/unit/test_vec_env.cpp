#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include <gygax/sim/vec_env.hpp>

using namespace gygax::sim;

namespace {

VecEnvConfig baseConfig() {
    VecEnvConfig c;
    c.numEnvs = 6;
    c.seed = 7;
    c.maxSteps = 60;
    return c;
}

std::vector<float> policy(const VecEnv& env, std::size_t step) {
    std::vector<float> a(env.numEnvs() * 2);
    for (std::size_t i = 0; i < env.numEnvs(); ++i) {
        a[i * 2] = 0.8F;
        a[i * 2 + 1] = std::sin(static_cast<float>(step + i) * 0.3F) * 0.6F;
    }
    return a;
}

}

TEST(VecEnv, ShapesAndRangesAreStable) {
    VecEnv env(baseConfig());
    EXPECT_EQ(env.observationSize(), 20U);
    ASSERT_EQ(env.observations().size(), 6U * 20U);
    for (std::size_t s = 0; s < 20; ++s) env.step(policy(env, s));
    for (std::size_t i = 0; i < env.numEnvs(); ++i) {
        for (int b = 0; b < 16; ++b) {
            const float v = env.observations()[i * 20 + static_cast<std::size_t>(b)];
            EXPECT_GE(v, 0.0F);
            EXPECT_LE(v, 1.0F);
        }
        EXPECT_TRUE(std::isfinite(env.rewards()[i]));
    }
}

TEST(VecEnv, SameSeedReproducesExactlyRegardlessOfThreadCount) {
    auto run = [](std::size_t threads) {
        auto cfg = baseConfig();
        cfg.threads = threads;
        cfg.randomization.motorGain = {0.8, 1.2};
        cfg.randomization.lidarNoiseStd = {0.0, 0.1};
        cfg.randomization.actionDelaySteps = {0, 3};
        VecEnv env(cfg);
        std::vector<float> trace;
        for (std::size_t s = 0; s < 150; ++s) {
            env.step(policy(env, s));
            trace.insert(trace.end(), env.observations().begin(), env.observations().end());
            trace.insert(trace.end(), env.rewards().begin(), env.rewards().end());
        }
        return trace;
    };
    const auto a = run(1);
    EXPECT_EQ(a, run(1));
    EXPECT_EQ(a, run(3));
    EXPECT_EQ(a, run(6));
}

TEST(VecEnv, DifferentSeedsDiffer) {
    auto cfg = baseConfig();
    VecEnv a(cfg);
    cfg.seed = 8;
    VecEnv b(cfg);
    EXPECT_NE(a.observations(), b.observations());
    a.reset(8);
    EXPECT_EQ(a.observations(), b.observations());
}

TEST(VecEnv, TruncatesAndAutoResetsAtTheStepLimit) {
    auto cfg = baseConfig();
    cfg.maxSteps = 5;
    cfg.autoReset = true;
    VecEnv env(cfg);
    std::vector<float> stop(env.numEnvs() * 2, 0.0F);
    for (int i = 0; i < 4; ++i) env.step(stop);
    for (auto t : env.truncated()) EXPECT_EQ(t, 0);
    env.step(stop);
    for (std::size_t i = 0; i < env.numEnvs(); ++i) EXPECT_EQ(env.truncated()[i], 1);
    EXPECT_EQ(env.episodesFinished(), env.numEnvs());
    env.step(stop);
    for (auto t : env.truncated()) EXPECT_EQ(t, 0);
}

TEST(VecEnv, ReachingTheGoalTerminatesWithReward) {
    auto cfg = baseConfig();
    cfg.numEnvs = 1;
    cfg.obstacles = 0;
    cfg.maxSteps = 2000;
    cfg.autoReset = false;
    cfg.goalRadius = 0.6;
    VecEnv env(cfg);
    bool reached = false;
    float bestReward = -100.0F;
    for (int s = 0; s < 2000 && !reached; ++s) {
        const float distance = env.observations()[16];
        const float bearing = env.observations()[17];
        std::vector<float> a{distance > 0.0F ? 0.6F * (1.0F - std::fabs(bearing)) : 0.0F, std::clamp(bearing * 2.0F, -1.0F, 1.0F)};
        env.step(a);
        bestReward = std::max(bestReward, env.rewards()[0]);
        reached = env.terminated()[0] != 0;
    }
    EXPECT_TRUE(reached);
    EXPECT_EQ(env.reachedGoal()[0], 1);
    EXPECT_GT(bestReward, 5.0F);
}

TEST(VecEnv, FinalObservationsSurviveAutoReset) {
    auto cfg = baseConfig();
    cfg.numEnvs = 2;
    cfg.maxSteps = 3;
    VecEnv env(cfg);
    std::vector<float> a(4, 0.5F);
    for (int i = 0; i < 3; ++i) env.step(a);
    ASSERT_EQ(env.truncated()[0], 1);
    EXPECT_NE(env.finalObservations(), env.observations());
}

TEST(VecEnv, ActionDelayPostponesMotion) {
    auto cfg = baseConfig();
    cfg.numEnvs = 1;
    cfg.obstacles = 0;
    cfg.randomization.actionDelaySteps = {3, 3};
    VecEnv env(cfg);
    const float before = env.observations()[18];
    std::vector<float> go{1.0F, 0.0F};
    for (int i = 0; i < 3; ++i) {
        env.step(go);
        EXPECT_EQ(env.observations()[18], before);
    }
    env.step(go);
    EXPECT_GT(env.observations()[18], before);
}

TEST(VecEnvConfig, ParsesValidJsonAndRejectsBadValues) {
    using gygax::sim::vecEnvConfigFromJson;
    std::string err;
    auto ok = vecEnvConfigFromJson(*gygax::json::parse(
                                       R"({"num_envs":32,"beams":8,"seed":5,"threads":4,"auto_reset":false,
                                           "randomization":{"motor_gain":[0.9,1.1],"action_delay_steps":[0,2]}})"),
                                   &err);
    ASSERT_TRUE(ok) << err;
    EXPECT_EQ(ok->numEnvs, 32U);
    EXPECT_EQ(ok->beams, 8);
    EXPECT_EQ(ok->seed, 5U);
    EXPECT_FALSE(ok->autoReset);
    EXPECT_DOUBLE_EQ(ok->randomization.motorGain.hi, 1.1);
    EXPECT_TRUE(vecEnvConfigFromJson(*gygax::json::parse("{}"), &err));

    auto bad = [&](const char* text, const char* needle) {
        EXPECT_FALSE(vecEnvConfigFromJson(*gygax::json::parse(text), &err)) << text;
        EXPECT_NE(err.find(needle), std::string::npos) << err;
    };
    bad("[]", "object");
    bad(R"({"num_envs":0})", "num_envs");
    bad(R"({"num_envs":100000})", "num_envs");
    bad(R"({"beams":"x"})", "beams");
    bad(R"({"obstacle_min_radius":2,"obstacle_max_radius":1})", "obstacle_min_radius");
    bad(R"({"randomization":{"motor_gain":[2,1]}})", "motor_gain");
    bad(R"({"randomization":{"lidar_dropout":[0,2]}})", "dropout");
    bad(R"({"randomization":{"action_delay_steps":[0,1000]}})", "delay");
    bad(R"({"randomization":{"wheel_base":[0,0.4]}})", "wheel_base");
    bad(R"({"randomization":{"motor_gain":[0,1]}})", "motor_gain");
}
