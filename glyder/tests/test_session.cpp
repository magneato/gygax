// A host and a client over loopback: discovery, pairing, the world model, and a full drop-in.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

#include <glyder/client.hpp>
#include <glyder/host.hpp>

using namespace glyder;
using namespace std::chrono_literals;

namespace {

bool waitFor(const std::function<bool()>& f, double seconds, const std::function<void()>& each = {}) {
    const double end = net::now() + seconds;
    while (net::now() < end) {
        if (each) each();
        if (f()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return f();
}

struct FakeSim final : Pilot {
    Host* host = nullptr; // a real simulation announces takeovers from inside the callbacks
    int busyFor = 10;
    int drives = 0, freshDrives = 0, staleDrives = 0;
    float lastForward = 0;
    bool taken = false, given = false;
    std::optional<std::string> refuse(std::uint32_t id) override {
        if (id == 99) return "a satellite cannot be flown";
        return std::nullopt;
    }
    std::optional<Wait> busy(std::uint32_t) override {
        if (busyFor > 0) {
            --busyFor;
            return Wait{"finishing a break turn", 0.4f};
        }
        return std::nullopt;
    }
    Handling take(std::uint32_t) override {
        taken = true;
        if (host) host->event(wire::EventKind::Pilot, "a pilot has the kite", 0);
        return {Handling::Model::Air, 15, 40, 0.8f, 6, 5};
    }
    void drive(std::uint32_t, const wire::Control& c, bool fresh, double) override {
        ++drives;
        (fresh ? freshDrives : staleDrives)++;
        if (fresh) lastForward = c.axes[0];
    }
    void give(std::uint32_t) override {
        given = true;
        if (host) host->event(wire::EventKind::Pilot, "the flock flies it again", 0);
    }
    bool exists(std::uint32_t id) override { return id == 7 || id == 99; }
};

std::vector<wire::EntityState> world(double t) {
    wire::EntityState kite;
    kite.id = 7;
    kite.shape = 0;
    kite.team = 1;
    kite.flags = wire::kAlive | wire::kDrivable;
    kite.pos = {static_cast<float>(t * 20.0), 100, 300};
    kite.vel = {20, 0, 0};
    wire::EntityState sat = kite;
    sat.id = 99;
    sat.pos = {0, 0, 550000};
    sat.vel = {7600, 0, 0};
    return {kite, sat};
}

} // namespace

TEST(GlyderSession, PairsShowsTheWorldAndHandsOverAtAStoppingPoint) {
    HostOptions o;
    o.name = "test";
    o.title = "TEST WORLD";
    o.bind = "127.0.0.1";
    o.port = 0;
    o.secret = "4242";
    o.snapshotHz = 60;
    o.controlTimeout = 0.15;
    o.releaseTimeout = 0.6;
    Host host(o);
    std::string error;
    ASSERT_TRUE(host.start(&error)) << error;
    EXPECT_EQ(host.pairingCode(), "4242");
    wire::Heightfield terrain;
    terrain.w = 40;
    terrain.h = 30;
    terrain.cell = 100;
    for (int i = 0; i < 40 * 30; ++i) terrain.z.push_back(static_cast<float>(i % 17));
    host.setTerrain(terrain);
    const double t0 = net::now();
    host.publish(0, world(0));

    Client client("glyder-test");
    ASSERT_TRUE(client.open(&error)) << error;
    const net::Address addr{0x7F000001u, host.port()};

    // Discovery.
    client.discover(true, {addr});
    ASSERT_TRUE(waitFor([&] { return !client.found().empty(); }, 3.0));
    const auto found = client.found();
    EXPECT_EQ(found[0].beacon.title, "TEST WORLD");
    EXPECT_TRUE(found[0].beacon.pairing);
    client.discover(false);

    // A wrong code is refused.
    client.connect(addr, "0000");
    ASSERT_TRUE(waitFor([&] { return client.state() == Client::State::Failed; }, 3.0));
    EXPECT_NE(client.error().find("pairing"), std::string::npos);

    // The right one gives the world, its terrain and live entities.
    client.connect(addr, "4242");
    FakeSim sim;
    sim.host = &host;
    auto tick = [&] {
        const double t = net::now() - t0;
        host.publish(t, world(t));
        host.service(sim, 0.01);
    };
    ASSERT_TRUE(waitFor([&] { return client.state() == Client::State::Connected; }, 3.0, tick));
    EXPECT_EQ(client.world()->title, "TEST WORLD");
    ASSERT_TRUE(waitFor([&] { return client.terrainProgress() >= 1.0f; }, 3.0, tick));
    const auto got = client.takeTerrain();
    ASSERT_TRUE(got);
    EXPECT_EQ(got->w, 40);
    EXPECT_NEAR(got->at(16, 0), 16.0f, 0.01f);
    std::vector<wire::EntityState> seen;
    ASSERT_TRUE(waitFor(
        [&] {
            client.sample(net::now(), seen);
            return seen.size() == 2;
        },
        3.0, tick));
    // Dead reckoning keeps the kite where it really is now, to within a frame.
    client.sample(net::now(), seen);
    const auto kite = std::find_if(seen.begin(), seen.end(), [](const auto& e) { return e.id == 7; });
    ASSERT_NE(kite, seen.end());
    EXPECT_NEAR(kite->pos.x, static_cast<float>((net::now() - t0) * 20.0), 2.0f);
    EXPECT_EQ(host.clients(), 1u);

    // A satellite cannot be flown.
    client.dropIn(99);
    std::vector<wire::DropState> states;
    auto collect = [&] {
        tick();
        for (auto& s : client.takeDropStates()) states.push_back(s);
    };
    ASSERT_TRUE(waitFor([&] { return !states.empty(); }, 3.0, collect));
    EXPECT_EQ(states.back().state, wire::Drop::Denied);
    states.clear();

    // The kite: riding while it finishes its break turn, then control.
    client.dropIn(7);
    ASSERT_TRUE(waitFor([&] { return !states.empty() && states.back().state == wire::Drop::Control; }, 3.0, collect));
    EXPECT_EQ(states.front().state, wire::Drop::Riding);
    EXPECT_EQ(states.front().reason, "finishing a break turn");
    EXPECT_EQ(states.back().model, static_cast<std::uint8_t>(Handling::Model::Air));
    EXPECT_FLOAT_EQ(states.back().maxSpeed, 40);
    EXPECT_TRUE(sim.taken);
    EXPECT_TRUE(host.driven(7));

    // The pilot's sticks arrive.
    wire::Control c;
    c.entity = 7;
    c.axes[0] = 0.8f;
    ASSERT_TRUE(waitFor([&] { return sim.freshDrives > 5 && std::fabs(sim.lastForward - 0.8f) < 1e-3f; }, 3.0,
                        [&] {
                            client.control(c);
                            collect();
                        }));

    // An order, as from gyde.
    client.order("posture wide");
    std::vector<std::string> orders;
    ASSERT_TRUE(waitFor(
        [&] {
            for (auto& s : host.takeOrders()) orders.push_back(s);
            return !orders.empty();
        },
        3.0, tick));
    EXPECT_EQ(orders[0], "posture wide");

    // The pilot goes quiet: the node holds, then autonomy takes it back.
    states.clear();
    ASSERT_TRUE(waitFor([&] { return !states.empty() && states.back().state == wire::Drop::Released; }, 3.0, collect));
    EXPECT_GT(sim.staleDrives, 0);
    EXPECT_TRUE(sim.given);
    EXPECT_NE(states.back().reason.find("link lost"), std::string::npos);
    EXPECT_FALSE(host.driven(7));

    client.disconnect();
    host.stop();
}
