#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>

#include <gygax/robotics/safety.hpp>

using namespace gygax::robotics;

TEST(VelocityShield, PassesSafeCommandsThrough) {
    ShieldConfig cfg;
    cfg.maxSpeed = 2.0;
    VelocityShield shield(cfg);
    shield.addObstacle({{10, 0}, 1.0});
    const auto r = shield.filter({0, 0}, {1.0, 0.5});
    EXPECT_FALSE(r.modified);
    EXPECT_NEAR(r.velocity.x, 1.0, 1e-12);
    EXPECT_NEAR(r.velocity.y, 0.5, 1e-12);
}

TEST(VelocityShield, ClampsSpeed) {
    ShieldConfig cfg;
    cfg.maxSpeed = 1.0;
    const auto r = VelocityShield(cfg).filter({0, 0}, {3.0, 4.0});
    EXPECT_TRUE(r.modified);
    EXPECT_NEAR(std::hypot(r.velocity.x, r.velocity.y), 1.0, 1e-9);
}

TEST(VelocityShield, RejectsInvalidLimitsObstaclesAndNonFiniteCommands) {
    ShieldConfig invalid;
    invalid.maxSpeed = 0.0;
    EXPECT_THROW((void)VelocityShield{invalid}, std::invalid_argument);

    VelocityShield shield;
    EXPECT_THROW((void)shield.addObstacle({{0.0, 0.0}, -1.0}), std::invalid_argument);
    EXPECT_THROW((void)shield.filter({0.0, 0.0}, {std::nan(""), 0.0}), std::invalid_argument);
    EXPECT_THROW((void)shield.filterAlongHeading({0.0, 0.0}, 0.0, std::numeric_limits<double>::infinity()), std::invalid_argument);
}

TEST(VelocityShield, RemovesTheInwardComponentAtTheBarrier) {
    ShieldConfig cfg;
    cfg.alpha = 1.0;
    cfg.margin = 0.0;
    cfg.bodyRadius = 0.0;
    cfg.maxSpeed = 5.0;
    VelocityShield shield(cfg);
    shield.addObstacle({{2, 0}, 1.0});
    const auto r = shield.filter({1.0, 0}, {1.0, 1.0});
    EXPECT_TRUE(r.modified);
    EXPECT_NEAR(r.velocity.x, 0.0, 1e-9);
    EXPECT_NEAR(r.velocity.y, 1.0, 1e-9);
    EXPECT_GE(r.activeConstraints, 1U);
}

TEST(VelocityShield, ClosedLoopNeverEntersTheObstacle) {
    ShieldConfig cfg;
    cfg.alpha = 3.0;
    cfg.margin = 0.05;
    cfg.bodyRadius = 0.25;
    cfg.maxSpeed = 1.5;
    VelocityShield shield(cfg);
    const CircleObstacle obstacle{{5, 0.05}, 0.8};
    shield.addObstacle(obstacle);
    Point2 p{0, 0};
    const double dt = 0.01;
    double closest = 1e9;
    for (int i = 0; i < 3000; ++i) {
        const auto r = shield.filter(p, {1.5, 0.0});
        p.x += r.velocity.x * dt;
        p.y += r.velocity.y * dt;
        closest = std::min(closest, std::hypot(p.x - obstacle.center.x, p.y - obstacle.center.y));
    }
    EXPECT_GE(closest, obstacle.radius + cfg.bodyRadius - 1e-6);
}

TEST(VelocityShield, KeepsTheBodyInsideGeofenceBounds) {
    ShieldConfig cfg;
    cfg.boundsMin = Point2{0, 0};
    cfg.boundsMax = Point2{10, 10};
    cfg.maxSpeed = 2.0;
    VelocityShield shield(cfg);
    Point2 p{5, 5};
    for (int i = 0; i < 5000; ++i) {
        const auto r = shield.filter(p, {2.0, 1.0});
        p.x += r.velocity.x * 0.01;
        p.y += r.velocity.y * 0.01;
    }
    EXPECT_LE(p.x, 10.0 - cfg.bodyRadius + 1e-6);
    EXPECT_LE(p.y, 10.0 - cfg.bodyRadius + 1e-6);
}

TEST(VelocityShield, HeadingFilterBrakesBeforeAWall) {
    ShieldConfig cfg;
    cfg.alpha = 2.0;
    cfg.maxSpeed = 1.0;
    VelocityShield shield(cfg);
    shield.addObstacle({{3, 0}, 1.0});
    Point2 p{0, 0};
    for (int i = 0; i < 4000; ++i) {
        const auto r = shield.filterAlongHeading(p, 0.0, 1.0);
        p.x += r.velocity.x * 0.01;
    }
    EXPECT_LT(p.x, 2.0 - cfg.bodyRadius + 1e-6);
    EXPECT_GT(p.x, 1.0);
    EXPECT_NEAR(shield.filterAlongHeading({0, 0}, 0.0, -0.5).velocity.x, -0.5, 1e-9);
}

TEST(JointShield, EnforcesPositionVelocityAndAcceleration) {
    JointShield shield({{-1.0, 1.0, 1.0, 5.0}});
    shield.reset({0.0});
    double prev = 0.0;
    double prevV = 0.0;
    for (int i = 0; i < 400; ++i) {
        const auto out = shield.filter({10.0}, 0.01);
        const double v = (out[0] - prev) / 0.01;
        EXPECT_LE(std::fabs(v), 1.0 + 1e-9);
        EXPECT_LE(std::fabs(v - prevV), 5.0 * 0.01 + 1e-9);
        EXPECT_LE(out[0], 1.0);
        prev = out[0];
        prevV = v;
    }
    EXPECT_NEAR(prev, 1.0, 1e-9);
    EXPECT_GT(shield.clippedCount(), 0U);
    const auto nan = shield.filter({std::nan("")}, 0.01);
    EXPECT_TRUE(std::isfinite(nan[0]));
}

TEST(JointShield, RejectsInvalidLimitsAndTimesteps) {
    EXPECT_THROW(JointShield({{1.0, -1.0, 1.0, 5.0}}), std::invalid_argument);
    JointShield shield({{-1.0, 1.0, 1.0, 5.0}});
    shield.reset({0.0});
    EXPECT_THROW((void)shield.filter({0.5}, 0.0), std::invalid_argument);
    EXPECT_THROW((void)shield.filter({0.5}, -0.01), std::invalid_argument);
    EXPECT_THROW(shield.reset({}), std::invalid_argument);
}

TEST(ActionChunkEnsembler, BlendsOverlappingChunks) {
    ActionChunkEnsembler ens(0.0);
    ens.push(0, {{1.0}, {1.0}, {1.0}});
    ens.push(1, {{3.0}, {3.0}, {3.0}});
    EXPECT_NEAR((*ens.action(0))[0], 1.0, 1e-12);
    EXPECT_NEAR((*ens.action(1))[0], 2.0, 1e-12);
    EXPECT_NEAR((*ens.action(2))[0], 2.0, 1e-12);
    EXPECT_NEAR((*ens.action(3))[0], 3.0, 1e-12);
    EXPECT_FALSE(ens.action(4));
    EXPECT_EQ(ens.pending(), 0U);
}

TEST(ActionChunkEnsembler, DecayFavoursTheOldestPrediction) {
    ActionChunkEnsembler ens(1.0);
    ens.push(0, {{0.0}, {0.0}});
    ens.push(1, {{10.0}});
    EXPECT_LT((*ens.action(1))[0], 5.0);
}

TEST(TargetHold, FallsBackWhenTheSlowLoopGoesQuiet) {
    TargetHold hold(0.5, {0.0, 0.0});
    EXPECT_TRUE(hold.stale(0.0));
    hold.publish({1.0, 2.0}, 1.0);
    EXPECT_EQ(hold.latest(1.2), (std::vector<double>{1.0, 2.0}));
    EXPECT_EQ(hold.latest(2.0), (std::vector<double>{0.0, 0.0}));
    hold.publish({3.0, 4.0}, 2.0);
    EXPECT_EQ(hold.latest(2.1)[0], 3.0);
    EXPECT_EQ(hold.publishCount(), 2U);
}

TEST(TargetHold, RejectsMalformedTargetsAndTreatsClockRollbackAsStale) {
    TargetHold hold(0.5, {0.0, 0.0});
    EXPECT_THROW(TargetHold(0.0, {0.0}), std::invalid_argument);
    EXPECT_THROW(hold.publish({1.0}, 1.0), std::invalid_argument);
    EXPECT_THROW(hold.publish({1.0, 2.0}, std::nan("")), std::invalid_argument);
    hold.publish({1.0, 2.0}, 2.0);
    EXPECT_TRUE(hold.stale(1.0));
    EXPECT_TRUE(hold.stale(std::nan("")));
    EXPECT_EQ(hold.latest(1.0), (std::vector<double>{0.0, 0.0}));
}
