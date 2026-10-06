#include <gtest/gtest.h>

#include <gygax/brain/taxonomy.hpp>
#include <gygax/core/json.hpp>
#include <gygax/core/agent.hpp>
#include <cmath>

#include <gygax/collective/consensus.hpp>
#include <gygax/transport/streams.hpp>

#include "support.hpp"

import gygax.core.base;
import gygax.messaging;
import gygax.tools;
import gygax.nodes.ether;
import gygax.world_model;

using namespace gygax;
using namespace gygax::taxonomy;

namespace {

json::Value telemetry(Brain& b) {
    return *json::parse(b.telemetry());
}

template <typename T> T& as(std::unique_ptr<Brain>& p) {
    return *dynamic_cast<T*>(p.get());
}

}

TEST(Taxonomy, VehicleDynamicsIntegrateDeterministically) {
    auto brain = Create(kVehicle, 1);
    ASSERT_TRUE(brain);
    auto& v = as<Mobility::Vehicle>(brain);
    v.setThrottle(100.0F);
    for (int i = 0; i < 100; ++i) brain->advance(0.01);
    const auto accelerating = telemetry(*brain);
    EXPECT_GT(accelerating.getDouble("speed_mps"), 3.5);
    EXPECT_LT(accelerating.getDouble("speed_mps"), 4.0);
    EXPECT_GT(accelerating.getDouble("x"), 1.5);
    EXPECT_NEAR(accelerating.getDouble("y"), 0.0, 1e-9);
    v.setThrottle(0.0F);
    v.applyBraking(100.0F);
    for (int i = 0; i < 100; ++i) brain->advance(0.01);
    EXPECT_DOUBLE_EQ(telemetry(*brain).getDouble("speed_mps"), 0.0);

    auto turning = Create(kVehicle, 9);
    auto& t = as<Mobility::Vehicle>(turning);
    t.setThrottle(60.0F);
    t.steer(20.0F);
    for (int i = 0; i < 300; ++i) turning->advance(0.01);
    EXPECT_GT(std::fabs(telemetry(*turning).getDouble("heading_rad")), 0.1);
}

TEST(Taxonomy, ClampsUnsafeCommands) {
    auto brain = Create(kVehicle, 2);
    auto& v = as<Mobility::Vehicle>(brain);
    v.steer(500.0F);
    v.setThrottle(-10.0F);
    v.applyBraking(1000.0F);
    const auto t = telemetry(*brain);
    EXPECT_DOUBLE_EQ(t.getDouble("steer_deg"), 35.0);
    EXPECT_DOUBLE_EQ(t.getDouble("throttle_pct"), 0.0);
    EXPECT_DOUBLE_EQ(t.getDouble("brake_pct"), 100.0);
}

TEST(Taxonomy, DroneFollowsItsFlightStateMachine) {
    auto brain = Create(kDrone, 3);
    auto& d = as<Aerospace::Drone>(brain);
    EXPECT_EQ(telemetry(*brain).getString("mode"), "grounded");
    d.setAltitude(20.0F);
    EXPECT_EQ(telemetry(*brain).getString("mode"), "grounded");
    d.takeoff();
    EXPECT_EQ(telemetry(*brain).getString("mode"), "flying");
    EXPECT_DOUBLE_EQ(telemetry(*brain).getDouble("target_altitude_m"), 10.0);
    for (int i = 0; i < 400; ++i) brain->advance(0.01);
    EXPECT_NEAR(telemetry(*brain).getDouble("altitude_m"), 10.0, 1e-9);
    d.hover();
    EXPECT_EQ(telemetry(*brain).getString("mode"), "hovering");
    d.land();
    EXPECT_EQ(telemetry(*brain).getString("mode"), "landing");
    EXPECT_DOUBLE_EQ(telemetry(*brain).getDouble("target_altitude_m"), 0.0);
    for (int i = 0; i < 600; ++i) brain->advance(0.01);
    EXPECT_EQ(telemetry(*brain).getString("mode"), "grounded");
    EXPECT_DOUBLE_EQ(telemetry(*brain).getDouble("altitude_m"), 0.0);
}

TEST(Taxonomy, WorkerThreadsAdvanceStateWithoutBusyWaiting) {
    auto brain = Create(kUAV, 4);
    auto& uav = as<Aerospace::UAV>(brain);
    brain->initialize();
    uav.setAltitude(1.0F);
    ASSERT_TRUE(support::waitUntil([&] { return telemetry(*brain).getDouble("altitude_m") > 0.05; }));
    brain->freeze(true);
    const auto frozen = telemetry(*brain).getInt("ticks");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(telemetry(*brain).getInt("ticks"), frozen);
    brain->freeze(false);
    ASSERT_TRUE(support::waitUntil([&] { return telemetry(*brain).getInt("ticks") > frozen; }));
    brain->shutdown();
    brain->pre_delete();
}

TEST(Taxonomy, MessagesAndEventsAreRecorded) {
    auto brain = Create(kMobileEdgeDevice, 5);
    messaging::Message m{9, 5, "wake up"};
    EXPECT_TRUE(brain->receiveMessage(m));
    brain->emitEvent(7, nullptr, 128);
    const auto t = telemetry(*brain);
    EXPECT_EQ(t.getInt("inbox"), 1);
    EXPECT_EQ(t.getInt("events_emitted"), 1);
    as<Edge::MobileEdgeDevice>(brain).requestComputeOffload(4096);
    as<Edge::MobileEdgeDevice>(brain).vibrate(5.0F);
    const auto after = telemetry(*brain);
    EXPECT_EQ(after.getInt("offloaded_bytes"), 4096);
    EXPECT_DOUBLE_EQ(after.getDouble("vibration"), 1.0);
}

TEST(Taxonomy, CapabilityModulesRunRegisteredTools) {
    tools::ToolRegistry::getInstance().registerTool("emb.rev", [](const std::string& s) { return std::string(s.rbegin(), s.rend()); });
    auto brain = Create(kCapability, 6);
    as<Computer::Capability>(brain).runTask("emb.rev stressed");
    EXPECT_EQ(telemetry(*brain).getString("last_result"), "desserts");
    as<Computer::Capability>(brain).runTask("emb.absent x");
    EXPECT_NE(telemetry(*brain).getString("last_result").find("unknown tool"), std::string::npos);
    tools::ToolRegistry::getInstance().unregisterTool("emb.rev");
}

TEST(Taxonomy, StreamsAttachByName) {
    auto brain = Create(kVessel, 7);
    EXPECT_EQ(brain->getStream("udp"), nullptr);
    brain->attachStream(std::shared_ptr<comm::Stream>(comm::CreateUdpStream()));
    EXPECT_NE(brain->getStream("udp"), nullptr);
}

TEST(Taxonomy, RegistryResolvesNamesAndTypes) {
    EXPECT_TRUE(Create(std::string("Marine::Vessel"), 1));
    EXPECT_TRUE(Create(std::string("high altitude platform"), 2));
    EXPECT_FALSE(Create(std::string("Unobtainium"), 3));
    for (auto type : {kCapability, kVehicle, kVessel, kUAV, kDrone, kHighAltitudePlatform, kOrbitalConstellation, kStationaryTrafficMonitor,
                      kMobileEdgeDevice}) {
        auto b = Create(type, 10);
        ASSERT_TRUE(b);
        EXPECT_TRUE(json::parse(b->telemetry()));
    }
    EXPECT_TRUE(CreateVehicleModule(11));
    EXPECT_TRUE(CreateMobileEdgeDeviceModule(12));
}

TEST(DefaultAgent, RecordsMessagesIntoItsEpisodicMemory) {
    auto agent = CreateDefaultAgent();
    ASSERT_TRUE(agent);
    EXPECT_TRUE(agent->receiveMessage(messaging::Message{1, agent->sid(), "remember this"}));
    agent->emitEvent(3, nullptr, 8);
    const auto mem = state::WorldModel::getInstance().snapshot<state::EpisodicMemory>(agent->sid());
    ASSERT_TRUE(mem);
    ASSERT_EQ(mem->eventLogs.size(), 2U);
    EXPECT_EQ(mem->eventLogs[0], "message from 1: remember this");
}

TEST(Consensus, AggregatesIndependentEvidenceForTheSameHypothesis) {
    auto& brain = CollectiveBrain::getInstance();
    brain.resetPool();
    EXPECT_EQ(brain.deriveConsensus(), "IDLE");
    brain.submitHypothesis({1, "A", 0.4F});
    brain.submitHypothesis({2, "B", 0.9F});
    brain.submitHypothesis({3, "A", 0.5F});
    EXPECT_EQ(brain.getPool().size(), 3U);
    EXPECT_EQ(brain.deriveConsensus(), "B");

    brain.submitHypothesis({4, "A", 0.6F});
    brain.submitHypothesis({5, "A", 0.6F});
    const auto ranked = brain.report();
    ASSERT_EQ(ranked.size(), 2U);
    EXPECT_EQ(ranked[0].content, "A");
    EXPECT_EQ(ranked[0].votes, 4U);
    EXPECT_NEAR(ranked[0].confidence, 1.0 - 0.6 * 0.5 * 0.4 * 0.4, 1e-6);
    EXPECT_EQ(brain.deriveConsensus(), "A");

    brain.submitHypothesis({6, "C", 7.0F});
    EXPECT_EQ(brain.report().front().content, "C");
    brain.resetPool();
}
