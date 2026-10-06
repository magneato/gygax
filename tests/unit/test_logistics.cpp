#include <gtest/gtest.h>

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <fstream>

#include <gygax/logistics/ledger.hpp>

using namespace gygax;
using namespace gygax::logistics;

namespace {

json::Value obj(const char* text) {
    return *json::parse(text);
}

std::string tempPath() {
    char name[] = "/tmp/gygax-ledger-XXXXXX";
    const int fd = mkstemp(name);
    close(fd);
    return name;
}

} // namespace

TEST(LogisticsLine, ParsesTheCompactFormat) {
    ParsedLine p;
    std::string err;
    ASSERT_TRUE(parseLine("-3 ba99x drone=quadcopter power=solar", p, &err)) << err;
    EXPECT_EQ(p.delta, -3);
    EXPECT_EQ(p.sku, "ba99x");
    EXPECT_EQ(p.attrs.at("drone"), "quadcopter");
    EXPECT_EQ(p.attrs.at("power"), "solar");
    EXPECT_EQ(formatLine(p.delta, p.sku, p.attrs), "-3 ba99x drone=quadcopter power=solar");
    ASSERT_TRUE(parseLine("+8 ba99x site=plant-1 reason=produced", p, &err));
    EXPECT_EQ(p.delta, 8);
    EXPECT_EQ(p.site, "plant-1");
    EXPECT_EQ(formatLine(8, "x", {}), "+8 x");
}

TEST(LogisticsLine, RejectsMalformedInput) {
    ParsedLine p;
    std::string err;
    EXPECT_FALSE(parseLine("", p, &err));
    EXPECT_FALSE(parseLine("3", p, &err));
    EXPECT_FALSE(parseLine("0 ba99x", p, &err));
    EXPECT_FALSE(parseLine("abc ba99x", p, &err));
    EXPECT_FALSE(parseLine("1 ba99x power", p, &err));
    EXPECT_FALSE(parseLine("1 ba99x guid=nope", p, &err));
    EXPECT_FALSE(parseLine("1 bad;sku", p, &err));
    EXPECT_FALSE(parseLine("1000000 ba99x", p, &err));
}

TEST(LogisticsGuid, IsVersion4AndUnique) {
    const auto a = newGuid();
    const auto b = newGuid();
    EXPECT_TRUE(validGuid(a));
    EXPECT_NE(a, b);
    EXPECT_EQ(a[14], '4');
}

TEST(LogisticsSince, AcceptsDurationsAndDates) {
    const std::int64_t now = 1'800'000'000'000;
    EXPECT_EQ(*parseSince(json::Value("7d"), now), now - 7 * 86400000);
    EXPECT_EQ(*parseSince(json::Value("2h"), now), now - 2 * 3600000);
    EXPECT_EQ(*parseSince(json::Value("2026-01-01"), now), 1767225600000);
    EXPECT_EQ(*parseSince(json::Value("2026-01-01T00:00:00Z"), now), 1767225600000);
    EXPECT_FALSE(parseSince(json::Value("2026-13-01"), now).has_value());
    EXPECT_FALSE(parseSince(json::Value("2026-01-01T00:00"), now).has_value());
    EXPECT_EQ(*parseSince(json::Value(1700000000), now), 1700000000000);
    EXPECT_EQ(*parseSince(json::Value(), now), 0);
    EXPECT_FALSE(parseSince(json::Value("soon"), now).has_value());
}

TEST(Ledger, ProductionCreatesGuidedUnitsAndLossMarksThemLost) {
    Ledger ledger;
    auto made = ledger.record(obj(R"({"line":"+8 ba99x drone=quadcopter power=solar"})"));
    ASSERT_TRUE(made.ok) << made.message;
    EXPECT_EQ(made.body.find("guids")->size(), 8U);
    auto lost = ledger.record(obj(R"({"line":"-3 ba99x drone=quadcopter power=solar"})"));
    ASSERT_TRUE(lost.ok) << lost.message;
    EXPECT_EQ(lost.body.getString("reason"), "lost");
    EXPECT_EQ(lost.body.getString("line"), "-3 ba99x drone=quadcopter power=solar");
    const auto summary = ledger.summary(0);
    const auto& row = summary.find("skus")->asArray().front();
    EXPECT_EQ(row.getInt("active"), 5);
    EXPECT_EQ(row.getInt("lost"), 3);
    EXPECT_EQ(row.getInt("gained_in_window"), 8);
    EXPECT_EQ(row.getInt("lost_in_window"), 3);
    ASSERT_EQ(summary.find("flow")->size(), 2U);
    EXPECT_EQ(ledger.units("ba99x", "lost", 100).size(), 3U);
}

TEST(Ledger, LossWithSpecificGuidsTargetsThoseUnits) {
    Ledger ledger;
    const auto guid = newGuid();
    ASSERT_TRUE(ledger.record(obj(("{\"delta\":1,\"sku\":\"ba99x\",\"guids\":[\"" + guid + "\"]}").c_str())).ok);
    ASSERT_TRUE(ledger.record(obj(R"({"delta":2,"sku":"ba99x"})")).ok);
    auto lost = ledger.record(obj(("{\"delta\":-1,\"sku\":\"ba99x\",\"guids\":[\"" + guid + "\"],\"reason\":\"crash\"}").c_str()));
    ASSERT_TRUE(lost.ok) << lost.message;
    const auto unit = ledger.unit(guid);
    ASSERT_TRUE(unit.has_value());
    EXPECT_EQ(unit->getString("status"), "retired");
    EXPECT_EQ(unit->getString("end_reason"), "crash");
    auto again = ledger.record(obj(("{\"delta\":-1,\"sku\":\"ba99x\",\"guids\":[\"" + guid + "\"]}").c_str()));
    EXPECT_EQ(again.status, 409);
    auto recovered = ledger.record(obj(("{\"delta\":1,\"sku\":\"ba99x\",\"guids\":[\"" + guid + "\"],\"reason\":\"recovered\"}").c_str()));
    ASSERT_TRUE(recovered.ok) << recovered.message;
    EXPECT_EQ(ledger.unit(guid)->getString("status"), "active");
}

TEST(Ledger, RefusesToLoseMoreThanIsInStock) {
    Ledger ledger;
    ASSERT_TRUE(ledger.record(obj(R"({"line":"+2 ba99x drone=quadcopter"})")).ok);
    auto over = ledger.record(obj(R"({"line":"-3 ba99x drone=quadcopter"})"));
    EXPECT_EQ(over.status, 409);
    EXPECT_EQ(over.code, "insufficient_stock");
    EXPECT_EQ(over.body.getInt("available"), 2);
    auto wrongAttr = ledger.record(obj(R"({"line":"-1 ba99x drone=hexacopter"})"));
    EXPECT_EQ(wrongAttr.status, 409);
    EXPECT_EQ(ledger.record(obj(R"({"line":"-1 unknown"})")).status, 409);
}

TEST(Ledger, ValidatesGuidsSitesAndDuplicates) {
    Ledger ledger;
    EXPECT_EQ(ledger.record(obj(R"({"delta":1,"sku":"a","guids":["bad"]})")).status, 400);
    EXPECT_EQ(ledger.record(obj(R"({"delta":2,"sku":"a","guids":["6f9619ff-8b86-4011-b42d-00c04fc964ff"]})")).status, 400);
    EXPECT_EQ(ledger.record(obj(R"({"delta":1,"sku":"a","site":"nowhere"})")).status, 404);
    EXPECT_EQ(ledger.record(obj(R"({"delta":0,"sku":"a"})")).status, 400);
    ASSERT_TRUE(ledger.record(obj(R"({"delta":1,"sku":"a","guids":["6f9619ff-8b86-4011-b42d-00c04fc964ff"]})")).ok);
    EXPECT_EQ(ledger.record(obj(R"({"delta":1,"sku":"a","guids":["6f9619ff-8b86-4011-b42d-00c04fc964ff"]})")).status, 409);
}

TEST(Ledger, JournalRestoresStateAndToleratesATornTail) {
    const auto path = tempPath();
    std::string guid;
    {
        Ledger ledger(path);
        ASSERT_TRUE(ledger.putModel(obj(R"({"sku":"ba99x","kind":"quadcopter","power":"solar","max_range_m":12000})")).ok);
        ASSERT_TRUE(ledger.putSite(obj(R"({"id":"plant-1","kind":"factory","latitude":47.4,"longitude":8.5})")).ok);
        auto made = ledger.record(obj(R"({"line":"+4 ba99x drone=quadcopter site=plant-1"})"));
        ASSERT_TRUE(made.ok);
        guid = made.body.find("guids")->asArray().front().asString();
        ASSERT_TRUE(ledger.record(obj(R"({"line":"-1 ba99x"})")).ok);
        ASSERT_TRUE(ledger.updateUnit(guid, obj(R"({"device":"uav-1","fuel":0.5,"latitude":47.0,"longitude":8.0})")).ok);
    }
    {
        std::ofstream torn(path, std::ios::app);
        torn << R"({"op":"event","id":"x","sku")";
    }
    Ledger restored(path);
    EXPECT_EQ(restored.eventCount(), 2U);
    EXPECT_EQ(restored.models().size(), 1U);
    EXPECT_EQ(restored.sites().size(), 1U);
    const auto unit = restored.unit(guid);
    ASSERT_TRUE(unit.has_value());
    EXPECT_EQ(unit->getString("device"), "uav-1");
    EXPECT_DOUBLE_EQ(unit->getDouble("fuel"), 0.5);
    ASSERT_TRUE(restored.record(obj(R"({"line":"+1 ba99x"})")).ok);
    Ledger third(path);
    EXPECT_EQ(third.eventCount(), 3U);
    EXPECT_EQ(third.summary(0).find("totals")->getInt("active"), 4);
    std::remove(path.c_str());
}

TEST(Ledger, CorruptMiddleOfJournalIsRejected) {
    const auto path = tempPath();
    {
        std::ofstream f(path);
        f << "not json\n"
          << R"({"op":"site_remove","id":"x"})" << "\n";
    }
    EXPECT_THROW(Ledger{path}, std::runtime_error);
    std::remove(path.c_str());
}

TEST(Ledger, UnitUpdatesAreValidated) {
    Ledger ledger;
    auto made = ledger.record(obj(R"({"line":"+1 ba99x"})"));
    const auto guid = made.body.find("guids")->asArray().front().asString();
    EXPECT_EQ(ledger.updateUnit(guid, obj(R"({"fuel":2})")).status, 400);
    EXPECT_EQ(ledger.updateUnit(guid, obj(R"({"latitude":1})")).status, 400);
    EXPECT_EQ(ledger.updateUnit(guid, obj(R"({"site":"nope"})")).status, 404);
    EXPECT_EQ(ledger.updateUnit(newGuid(), obj(R"({"fuel":1})")).status, 404);
}

TEST(Geo, DistanceAndBearingProjectionAgree) {
    const Point zurich{47.3769, 8.5417};
    const Point bern{46.9480, 7.4474};
    const double d = distanceMeters(zurich, bern);
    EXPECT_NEAR(d, 95000.0, 2000.0);
    const auto mid = pointToward(zurich, bern, d / 2);
    EXPECT_NEAR(distanceMeters(zurich, mid), d / 2, 5.0);
    EXPECT_NEAR(distanceMeters(mid, bern), d / 2, 5.0);
    EXPECT_NEAR(distanceMeters(zurich, zurich), 0.0, 1e-6);
}

namespace {

Site site(const char* id, double lat, double lon, std::vector<std::string> services = {}) {
    Site s;
    s.id = id;
    s.kind = "refuel";
    s.position = {lat, lon};
    s.services = std::move(services);
    return s;
}

} // namespace

TEST(Planner, DirectFlightWithinRangeNeedsNoStops) {
    PlanRequest r;
    r.origin = {0.0, 0.0};
    r.waypoints = {{0.0, 0.05}};
    r.maxRangeM = 20000;
    r.cruiseMps = 10;
    const auto plan = planRoute(r, {});
    EXPECT_TRUE(plan.getBool("feasible"));
    EXPECT_EQ(plan.getInt("refuel_stops"), 0);
    EXPECT_NEAR(plan.getDouble("total_distance_m"), 5566.0, 20.0);
    EXPECT_NEAR(plan.getDouble("eta_seconds"), 556.6, 2.0);
}

TEST(Planner, InsertsRefuelStopsWhenTheTargetIsBeyondRange) {
    PlanRequest r;
    r.origin = {0.0, 0.0};
    r.waypoints = {{0.0, 0.3}};
    r.maxRangeM = 20000;
    const std::vector<Site> sites = {site("mid", 0.0, 0.15), site("far", 5.0, 5.0)};
    const auto plan = planRoute(r, sites);
    ASSERT_TRUE(plan.getBool("feasible")) << plan.dump();
    EXPECT_EQ(plan.getInt("refuel_stops"), 1);
    const auto& legs = plan.find("legs")->asArray();
    ASSERT_EQ(legs.size(), 2U);
    EXPECT_EQ(legs[0].getString("type"), "refuel");
    EXPECT_EQ(legs[0].getString("site"), "mid");
    EXPECT_EQ(legs[1].getString("type"), "waypoint");
}

TEST(Planner, ReportsInfeasibleRoutesAndSuggestsARefuelPoint) {
    PlanRequest r;
    r.origin = {0.0, 0.0};
    r.waypoints = {{0.0, 1.0}};
    r.maxRangeM = 20000;
    const auto plan = planRoute(r, {});
    EXPECT_FALSE(plan.getBool("feasible"));
    const auto* s = plan.find("suggestion");
    ASSERT_NE(s, nullptr);
    const auto* p = s->find("add_refuel_point");
    ASSERT_NE(p, nullptr);
    const Point suggested{p->getDouble("latitude"), p->getDouble("longitude")};
    EXPECT_NEAR(distanceMeters({0, 0}, suggested), 18000.0, 50.0);

    const std::vector<Site> sites = {site("added", suggested.latitude, suggested.longitude)};
    const auto second = planRoute(r, sites);
    EXPECT_FALSE(second.getBool("feasible"));
    const auto* again = second.find("suggestion")->find("add_refuel_point");
    ASSERT_NE(again, nullptr);
    EXPECT_NEAR(distanceMeters({0, 0}, Point{again->getDouble("latitude"), again->getDouble("longitude")}), 36000.0, 100.0);
}

TEST(Planner, ReturnLegMustBeReachableAndPowerTypesMatch) {
    PlanRequest r;
    r.origin = {0.0, 0.0};
    r.waypoints = {{0.0, 0.12}};
    r.returnTo = Point{0.0, 0.0};
    r.maxRangeM = 20000;
    r.power = "solar";
    const auto plan = planRoute(r, {});
    EXPECT_FALSE(plan.getBool("feasible"));

    const std::vector<Site> wrongPower = {site("bat", 0.0, 0.06, {"battery"})};
    EXPECT_FALSE(planRoute(r, wrongPower).getBool("feasible"));
    const std::vector<Site> rightPower = {site("sun", 0.0, 0.06, {"solar", "battery"})};
    const auto ok = planRoute(r, rightPower);
    EXPECT_TRUE(ok.getBool("feasible")) << ok.dump();
    EXPECT_GE(ok.getInt("refuel_stops"), 1);
}

TEST(Planner, LowFuelAndReserveAreHonoured) {
    PlanRequest r;
    r.origin = {0.0, 0.0};
    r.waypoints = {{0.0, 0.1}};
    r.maxRangeM = 20000;
    r.fuelFraction = 0.5;
    EXPECT_FALSE(planRoute(r, {}).getBool("feasible"));
    r.fuelFraction = 0.7;
    const auto ok = planRoute(r, {});
    EXPECT_TRUE(ok.getBool("feasible"));
    EXPECT_GT(ok.getDouble("final_fuel_fraction"), 0.1);
    PlanRequest bad;
    bad.waypoints = {{0, 1}};
    EXPECT_FALSE(planRoute(bad, {}).getBool("feasible"));
}
