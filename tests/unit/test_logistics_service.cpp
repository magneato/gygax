#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>

#include <gygax/core/log.hpp>
#include <gygax/net/http.hpp>
#include <gygax/service/service.hpp>

#include "support.hpp"

using namespace gygax;

namespace {

class LogisticsServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        log::setLevel(log::Level::Error);
        char name[] = "/tmp/gygax-svc-ledger-XXXXXX";
        const int fd = mkstemp(name);
        close(fd);
        journal = name;
        startService();
    }

    void TearDown() override {
        svc.reset();
        std::remove(journal.c_str());
    }

    void startService() {
        service::ServiceConfig c;
        c.port = 0;
        c.token = "tok";
        c.engines = {"echo"};
        c.ledgerPath = journal;
        svc = std::make_unique<service::Service>(c);
        std::string err;
        ASSERT_TRUE(svc->start(&err)) << err;
        base = "http://127.0.0.1:" + std::to_string(svc->port());
    }

    net::ClientResponse call(const std::string& method, const std::string& path, const std::string& body = {}) {
        return net::httpRequest(*net::Url::parse(base + path), method, body,
                                {{"Authorization", "Bearer tok"}, {"Content-Type", "application/json"}});
    }

    json::Value jsonOf(const net::ClientResponse& r) { return *json::parse(r.body); }

    std::unique_ptr<service::Service> svc;
    std::string base;
    std::string journal;
};

} // namespace

TEST_F(LogisticsServiceTest, RecordsFlowSummarisesItAndSurvivesARestart) {
    ASSERT_EQ(
        call("POST", "/v1/logistics/models", R"({"sku":"ba99x","kind":"quadcopter","power":"solar","max_range_m":20000,"cruise_mps":12})")
            .status,
        201);
    ASSERT_EQ(call("POST", "/v1/logistics/sites", R"({"id":"plant-1","kind":"factory","latitude":47.0,"longitude":8.0})").status, 201);
    const auto made = call("POST", "/v1/logistics/events", R"({"line":"+8 ba99x drone=quadcopter power=solar site=plant-1"})");
    ASSERT_EQ(made.status, 201) << made.body;
    const auto lost = call("POST", "/v1/logistics/events", R"({"line":"-3 ba99x drone=quadcopter power=solar"})");
    ASSERT_EQ(lost.status, 201) << lost.body;
    EXPECT_EQ(jsonOf(lost).getString("line"), "-3 ba99x drone=quadcopter power=solar");
    EXPECT_EQ(call("POST", "/v1/logistics/events", R"({"line":"-9 ba99x"})").status, 409);
    EXPECT_EQ(call("POST", "/v1/logistics/events", R"({"line":"nonsense"})").status, 400);

    auto summary = jsonOf(call("GET", "/v1/logistics/summary?since=1d"));
    EXPECT_EQ(summary.find("totals")->getInt("active"), 5);
    EXPECT_EQ(summary.find("totals")->getInt("lost"), 3);
    EXPECT_EQ(jsonOf(call("GET", "/v1/logistics/units?sku=ba99x&status=lost")).find("units")->size(), 3U);
    EXPECT_EQ(jsonOf(call("GET", "/v1/logistics/events?sku=ba99x")).find("events")->size(), 2U);
    EXPECT_EQ(call("GET", "/v1/logistics/summary?since=garbage").status, 400);

    const auto metrics = call("GET", "/metrics").body;
    EXPECT_NE(metrics.find("gygax_logistics_units{sku=\"ba99x\",status=\"lost\"} 3"), std::string::npos) << metrics;

    svc.reset();
    startService();
    summary = jsonOf(call("GET", "/v1/logistics/summary?since=1d"));
    EXPECT_EQ(summary.find("totals")->getInt("active"), 5);
    EXPECT_EQ(jsonOf(call("GET", "/v1/logistics/sites")).find("sites")->size(), 1U);
}

TEST_F(LogisticsServiceTest, PlansRoutesUsingTheModelRangeAndRegisteredRefuelSites) {
    ASSERT_EQ(call("POST", "/v1/logistics/models", R"({"sku":"ba99x","power":"solar","max_range_m":20000,"cruise_mps":10})").status, 201);
    const auto made = jsonOf(call("POST", "/v1/logistics/events", R"({"line":"+1 ba99x"})"));
    const auto guid = made.find("guids")->asArray().front().asString();
    ASSERT_EQ(call("POST", "/v1/logistics/units/" + guid, R"({"latitude":0.0,"longitude":0.0,"fuel":1.0})").status, 200);

    const std::string far = R"({"unit":")" + guid + R"(","waypoints":[{"latitude":0.0,"longitude":0.3}]})";
    auto blocked = jsonOf(call("POST", "/v1/logistics/plan", far));
    EXPECT_FALSE(blocked.getBool("feasible"));
    ASSERT_NE(blocked.find("suggestion"), nullptr);

    ASSERT_EQ(call("POST", "/v1/logistics/sites", R"({"id":"mid","latitude":0.0,"longitude":0.15,"services":["solar"]})").status, 201);
    auto plan = jsonOf(call("POST", "/v1/logistics/plan", far));
    EXPECT_TRUE(plan.getBool("feasible")) << plan.dump();
    EXPECT_EQ(plan.getInt("refuel_stops"), 1);
    EXPECT_EQ(plan.find("route")->size(), 2U);
    EXPECT_EQ(plan.find("inputs")->getString("fuel_source"), "ledger");

    EXPECT_EQ(call("POST", "/v1/logistics/plan", R"({"origin":{"latitude":0,"longitude":0},"waypoints":[{"latitude":0,"longitude":0.01}]})")
                  .status,
              400);
    EXPECT_EQ(call("POST", "/v1/logistics/plan",
                   R"({"unit":"6f9619ff-8b86-4011-b42d-00c04fc964ff","waypoints":[{"latitude":0,"longitude":0.01}]})")
                  .status,
              404);
    EXPECT_EQ(call("DELETE", "/v1/logistics/sites/mid").status, 200);
    EXPECT_EQ(call("DELETE", "/v1/logistics/sites/mid").status, 404);
}

TEST_F(LogisticsServiceTest, AgentsGetLogisticsToolsAndJsonRpcWorks) {
    const auto out = jsonOf(call("POST", "/v1/tools/logistics.record/invoke", R"({"input":"+2 ba99x drone=quadcopter"})"));
    EXPECT_NE(out.getString("output").find("\"delta\":2"), std::string::npos);
    const auto summary = jsonOf(call("POST", "/v1/tools/logistics.summary/invoke", R"({"input":"1d"})"));
    EXPECT_NE(summary.getString("output").find("\"active\":2"), std::string::npos);
    const auto rpc = jsonOf(call("POST", "/rpc", R"({"jsonrpc":"2.0","id":1,"method":"logistics.summary","params":{"since":"1d"}})"));
    EXPECT_EQ(rpc.find("result")->find("totals")->getInt("active"), 2);
}
