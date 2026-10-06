#include <gtest/gtest.h>

#include <gygax/net/link.hpp>
#include <gygax/robotics/mavlink.hpp>
#include <gygax/robotics/sim_vehicle.hpp>

#include "support.hpp"

using namespace gygax;
using namespace gygax::robotics;
using namespace std::chrono_literals;

namespace {

constexpr auto kHeartbeatTimeout = 3000ms;
constexpr auto kMissionProgressTimeout = 8000ms;

}

TEST(VirtualAutopilot, FliesATakeoffGotoAndLandMissionThroughTheVehicleClient) {
    auto [a, b] = net::makeLinkPair();
    VirtualAutopilotOptions options;
    options.climbRate = 40.0;
    options.cruiseSpeed = 80.0;
    options.descentRate = 40.0;
    VirtualAutopilot pilot(a, options);
    pilot.start();
    mavlink::Vehicle vehicle(b);
    vehicle.start();
    ASSERT_TRUE(vehicle.waitForHeartbeat(kHeartbeatTimeout));

    EXPECT_NE(vehicle.takeoff(10.0), 0);
    EXPECT_FALSE(pilot.state().flying);
    ASSERT_EQ(vehicle.setMode(VirtualAutopilot::kModeGuided), 0);
    ASSERT_EQ(vehicle.arm(), 0);
    ASSERT_TRUE(support::waitUntil([&] { return vehicle.state().armed; }));
    ASSERT_EQ(vehicle.takeoff(10.0), 0);
    ASSERT_TRUE(support::waitUntil([&] { return vehicle.state().altitudeRelative > 9.5; }));

    const double lat = pilot.state().latitude + 0.0002;
    ASSERT_EQ(vehicle.gotoGlobal(lat, pilot.state().longitude, 10.0), 0);
    ASSERT_TRUE(support::waitUntil([&] { return std::abs(vehicle.state().latitude - lat) < 0.00005; }, kMissionProgressTimeout));

    ASSERT_EQ(vehicle.land(), 0);
    ASSERT_TRUE(support::waitUntil([&] { return !vehicle.state().armed; }, kMissionProgressTimeout));
    EXPECT_LT(vehicle.state().altitudeRelative, 0.5);
    EXPECT_GE(pilot.commandsHandled(), 5U);
    vehicle.stop();
    pilot.stop();
}

TEST(VirtualAutopilot, RejectsDisarmWhileFlyingAndServesParameters) {
    auto [a, b] = net::makeLinkPair();
    VirtualAutopilotOptions options;
    options.climbRate = 40.0;
    VirtualAutopilot pilot(a, options);
    pilot.start();
    mavlink::Vehicle vehicle(b);
    vehicle.start();
    ASSERT_TRUE(vehicle.waitForHeartbeat(kHeartbeatTimeout));
    ASSERT_EQ(vehicle.setMode(VirtualAutopilot::kModeGuided), 0);
    ASSERT_EQ(vehicle.arm(), 0);
    ASSERT_EQ(vehicle.takeoff(5.0), 0);
    ASSERT_TRUE(support::waitUntil([&] { return pilot.state().flying; }));
    EXPECT_NE(vehicle.disarm(), 0);
    EXPECT_TRUE(pilot.state().armed);

    double value = 0.0;
    ASSERT_EQ(vehicle.readParameter("WPNAV_SPEED", value), 0);
    EXPECT_DOUBLE_EQ(value, 500.0);
    ASSERT_EQ(vehicle.setParameter("WPNAV_SPEED", 750.0), 0);
    ASSERT_EQ(vehicle.readParameter("WPNAV_SPEED", value), 0);
    EXPECT_DOUBLE_EQ(value, 750.0);
    vehicle.stop();
    pilot.stop();
}

TEST(VirtualAutopilot, HonoursMessageIntervalRequests) {
    auto [a, b] = net::makeLinkPair();
    VirtualAutopilot pilot(a);
    pilot.start();
    mavlink::Vehicle vehicle(b);
    vehicle.start();
    ASSERT_TRUE(vehicle.waitForHeartbeat(kHeartbeatTimeout));
    ASSERT_EQ(vehicle.requestMessageRate(74, 25.0), 0);
    ASSERT_TRUE(support::waitUntil([&] { return vehicle.lastMessage("VFR_HUD").has_value(); }));
    vehicle.stop();
    pilot.stop();
}
