#include <gtest/gtest.h>

#include <cmath>

#include <gygax/sim/world2d.hpp>

#include "support.hpp"

using namespace gygax::sim;

TEST(World2D, StraightDriveCoversTheExpectedDistance) {
    World2D world(50, 50, 1);
    AgentConfig cfg;
    cfg.name = "r";
    cfg.start = {5, 25, 0.0};
    const auto h = world.spawn(cfg);
    ASSERT_TRUE(world.setWheelSpeeds(h, 1.0, 1.0));
    for (int i = 0; i < 100; ++i) world.step(0.02);
    const auto p = *world.pose(h);
    EXPECT_NEAR(p.x, 7.0, 1e-6);
    EXPECT_NEAR(p.y, 25.0, 1e-6);
    EXPECT_NEAR(world.agent(h)->distance, 2.0, 1e-6);
    EXPECT_NEAR(world.time(), 2.0, 1e-9);
}

TEST(World2D, DifferentialDriveFollowsAnExactArc) {
    World2D world(50, 50, 1);
    AgentConfig cfg;
    cfg.wheelBase = 0.4;
    cfg.start = {10, 10, 0.0};
    const auto h = world.spawn(cfg);
    world.setWheelSpeeds(h, 0.8, 1.2);
    const double v = 1.0;
    const double w = (1.2 - 0.8) / 0.4;
    constexpr double total = 2.0;
    for (int i = 0; i < 200; ++i) world.step(0.01);
    const auto p = *world.pose(h);
    EXPECT_NEAR(p.x, 10.0 + v / w * std::sin(w * total), 1e-3);
    EXPECT_NEAR(p.y, 10.0 + v / w * (1.0 - std::cos(w * total)), 1e-3);
    EXPECT_NEAR(p.theta, std::remainder(w * total, 2.0 * M_PI), 1e-6);
}

TEST(World2D, SpinInPlaceKeepsPosition) {
    World2D world(20, 20, 1);
    AgentConfig cfg;
    cfg.start = {10, 10, 0.0};
    const auto h = world.spawn(cfg);
    world.setWheelSpeeds(h, -0.5, 0.5);
    for (int i = 0; i < 100; ++i) world.step(0.02);
    const auto p = *world.pose(h);
    EXPECT_NEAR(p.x, 10.0, 1e-9);
    EXPECT_NEAR(p.y, 10.0, 1e-9);
    EXPECT_GT(std::fabs(p.theta), 0.1);
}

TEST(World2D, WheelCommandsAreClamped) {
    World2D world(20, 20, 1);
    AgentConfig cfg;
    cfg.maxWheelSpeed = 1.5;
    const auto h = world.spawn(cfg);
    world.setWheelSpeeds(h, 100.0, -100.0);
    EXPECT_DOUBLE_EQ(world.agent(h)->leftCommand, 1.5);
    EXPECT_DOUBLE_EQ(world.agent(h)->rightCommand, -1.5);
    EXPECT_FALSE(world.setWheelSpeeds(999, 1, 1));
}

TEST(World2D, WallsAndObstaclesStopMotionWithoutOverlap) {
    World2D world(10, 10, 1);
    world.addObstacle({{6.0, 5.0}, 0.5});
    AgentConfig cfg;
    cfg.radius = 0.25;
    cfg.start = {2.0, 5.0, 0.0};
    const auto h = world.spawn(cfg);
    world.setWheelSpeeds(h, 1.5, 1.5);
    for (int i = 0; i < 300; ++i) world.step(0.02);
    const auto p = *world.pose(h);
    EXPECT_LE(p.x, 6.0 - 0.5 - 0.25 + 1e-6);
    EXPECT_FALSE(world.anyOverlap());
    EXPECT_EQ(world.totalCollisions(), 1U);
    EXPECT_GT(world.agent(h)->contactSteps, 10U);

    World2D walled(10, 10, 1);
    const auto w = walled.spawn(AgentConfig{"w", 0.25, 0.4, 1.5, {5, 5, M_PI / 2}});
    walled.setWheelSpeeds(w, 1.5, 1.5);
    for (int i = 0; i < 400; ++i) walled.step(0.02);
    EXPECT_LE(walled.pose(w)->y, 10.0 - 0.25 + 1e-9);
}

TEST(World2D, AgentsCollideWithEachOther) {
    World2D world(20, 20, 1);
    const auto a = world.spawn(AgentConfig{"a", 0.3, 0.4, 1.5, {5, 10, 0}});
    const auto b = world.spawn(AgentConfig{"b", 0.3, 0.4, 1.5, {8, 10, M_PI}});
    world.setWheelSpeeds(a, 1.0, 1.0);
    world.setWheelSpeeds(b, 1.0, 1.0);
    for (int i = 0; i < 300; ++i) world.step(0.02);
    EXPECT_FALSE(world.anyOverlap());
    EXPECT_GE(std::hypot(world.pose(a)->x - world.pose(b)->x, world.pose(a)->y - world.pose(b)->y), 0.6 - 1e-6);
}

TEST(World2D, LidarMeasuresDistancesToObstaclesWallsAndAgents) {
    World2D world(20, 20, 1);
    world.addObstacle({{10.0, 5.0}, 1.0});
    const auto h = world.spawn(AgentConfig{"s", 0.25, 0.4, 1.5, {5.0, 5.0, 0.0}});
    LidarConfig one;
    one.beams = 1;
    one.maxRange = 30.0;
    auto ahead = world.lidar(h, one);
    ASSERT_EQ(ahead.size(), 1U);
    EXPECT_NEAR(ahead[0], 4.0, 1e-6);

    world.spawn(AgentConfig{"blocker", 0.5, 0.4, 1.5, {5.0, 8.0, 0.0}});
    World2D fresh(20, 20, 1);
    const auto s = fresh.spawn(AgentConfig{"s", 0.25, 0.4, 1.5, {5.0, 5.0, M_PI / 2}});
    fresh.spawn(AgentConfig{"o", 0.5, 0.4, 1.5, {5.0, 9.0, 0.0}});
    auto up = fresh.lidar(s, one);
    EXPECT_NEAR(up[0], 3.5, 1e-6);

    World2D open(20, 20, 1);
    const auto t = open.spawn(AgentConfig{"t", 0.25, 0.4, 1.5, {5.0, 5.0, M_PI}});
    EXPECT_NEAR(open.lidar(t, one)[0], 5.0, 1e-6);

    LidarConfig capped;
    capped.beams = 1;
    capped.maxRange = 2.0;
    EXPECT_NEAR(world.lidar(h, capped)[0], 2.0, 1e-6);
    LidarConfig fan;
    fan.beams = 16;
    EXPECT_EQ(world.lidar(h, fan).size(), 16U);
    EXPECT_TRUE(world.lidar(999, fan).empty());
}

TEST(World2D, LidarNoiseIsBoundedAndSeeded) {
    auto sample = [](std::uint64_t seed) {
        World2D world(20, 20, seed);
        const auto h = world.spawn(AgentConfig{"n", 0.25, 0.4, 1.5, {5, 5, 0}});
        LidarConfig cfg;
        cfg.beams = 8;
        cfg.noiseStd = 0.1;
        cfg.maxRange = 8.0;
        return world.lidar(h, cfg);
    };
    EXPECT_EQ(sample(3), sample(3));
    EXPECT_NE(sample(3), sample(4));
    for (const float r : sample(3)) {
        EXPECT_GE(r, 0.0F);
        EXPECT_LE(r, 8.0F);
    }
}

TEST(World2D, RandomObstaclesKeepClearOfAgentsAndEachOther) {
    World2D world(24, 24, 5);
    world.spawn(AgentConfig{"a", 0.25, 0.4, 1.5, {3, 3, 0}});
    world.addRandomObstacles(10, 0.4, 1.0, 2.0);
    ASSERT_FALSE(world.obstacles().empty());
    EXPECT_FALSE(world.anyOverlap());
    for (const auto& o : world.obstacles()) {
        EXPECT_GE(std::hypot(o.center.x - 3, o.center.y - 3), o.radius + 0.25 + 2.0 - 1e-9);
        EXPECT_GE(o.center.x - o.radius, -1e-9);
        EXPECT_LE(o.center.x + o.radius, 24.0 + 1e-9);
    }
}

TEST(World2D, RealHardwareTargetRequiresALinkAndUsesTelemetry) {
    World2D world(20, 20, 1);
    const auto h = world.spawn(AgentConfig{"Rover_0", 0.25, 0.4, 1.5, {5, 5, 0}});
    EXPECT_FALSE(world.setRunTarget(h, RunTarget::RealHardware));

    auto robot = gygax::comm::CreateUdpStream(47611);
    auto host = std::shared_ptr<gygax::comm::Stream>(gygax::comm::CreateUdpStream(47612));
    world.attachLink(std::make_shared<StreamRobotLink>(host, "127.0.0.1:47611"));
    ASSERT_TRUE(world.setRunTarget(h, RunTarget::RealHardware));
    EXPECT_EQ(world.runTarget(h), RunTarget::RealHardware);
    ASSERT_TRUE(world.setWheelSpeeds(h, 0.4, 0.6));

    std::vector<std::string> commands;
    ASSERT_TRUE(gygax::support::waitUntil([&] {
        while (auto m = robot->receive()) commands.push_back(*m);
        return commands.size() >= 2;
    }));
    const auto drive = *gygax::json::parse(commands.back());
    EXPECT_EQ(drive.getString("cmd"), "drive");
    EXPECT_EQ(drive.getString("agent"), "Rover_0");
    EXPECT_DOUBLE_EQ(drive.getDouble("left"), 0.4);
    EXPECT_DOUBLE_EQ(drive.getDouble("right"), 0.6);

    (void)robot->transmit("127.0.0.1:47612", R"({"agent":"Rover_0","seq":5,"pose":{"x":12.5,"y":7.0,"theta":1.0},"lidar":[1.5,2.5]})");
    ASSERT_TRUE(gygax::support::waitUntil([&] {
        world.step(0.02);
        return world.agent(h)->remote.has_value();
    }));
    EXPECT_DOUBLE_EQ(world.pose(h)->x, 12.5);
    EXPECT_EQ(world.agent(h)->remote->sequence, 5U);
    LidarConfig cfg;
    EXPECT_EQ(world.lidar(h, cfg), (std::vector<float>{1.5F, 2.5F}));
    ASSERT_TRUE(world.setRunTarget(h, RunTarget::Simulated));
}

TEST(World2D, UsdaExportIsWellFormedAndAnimated) {
    World2D world(10, 10, 1);
    world.setRecording(true, 1);
    world.addObstacle({{5, 5}, 0.5});
    const auto h = world.spawn(AgentConfig{"Rover_0", 0.25, 0.4, 1.5, {2, 2, 0}});
    world.setWheelSpeeds(h, 1, 1);
    for (int i = 0; i < 5; ++i) world.step(0.1);
    const auto usd = world.toUsda();
    EXPECT_EQ(usd.rfind("#usda 1.0", 0), 0U);
    EXPECT_NE(usd.find("defaultPrim = \"World\""), std::string::npos);
    EXPECT_NE(usd.find("def Xform \"Rover_0\""), std::string::npos);
    EXPECT_NE(usd.find("def Cylinder \"Obstacle_0\""), std::string::npos);
    EXPECT_NE(usd.find("xformOp:translate.timeSamples"), std::string::npos);
    EXPECT_NE(usd.find("endTimeCode = 5"), std::string::npos);
    int open = 0;
    int close = 0;
    for (const char c : usd) {
        open += c == '{';
        close += c == '}';
    }
    EXPECT_EQ(open, close);

    World2D still(10, 10, 1);
    still.spawn(AgentConfig{"S", 0.25, 0.4, 1.5, {1, 1, 0}});
    const auto stillUsd = still.toUsda();
    EXPECT_EQ(stillUsd.find("timeSamples"), std::string::npos);
    EXPECT_NE(stillUsd.find("xformOp:rotateZ = 0"), std::string::npos);
}

TEST(World2D, SnapshotDescribesAgents) {
    World2D world(10, 10, 1);
    world.spawn(AgentConfig{"A", 0.25, 0.4, 1.5, {1, 2, 0.5}});
    const auto snap = world.snapshot();
    ASSERT_EQ(snap.find("agents")->asArray().size(), 1U);
    EXPECT_EQ(snap.find("agents")->asArray()[0].getString("name"), "A");
    EXPECT_EQ(snap.find("agents")->asArray()[0].getString("target"), "Simulated");
}
