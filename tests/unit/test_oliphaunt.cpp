#include <gtest/gtest.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include <gygax/oliphaunt/memory.hpp>

using namespace gygax::oliphaunt;

namespace {

constexpr std::int64_t kDay = 86400;
// Sightings must be inside the retention window, so the scenarios start twenty days ago.
// Aligned to a UTC midnight (plus an hour) so a scenario's "same day" never straddles a date change.
const std::int64_t kT0 = (std::time(nullptr) / kDay - 20) * kDay + 3600;

Sighting car(const char* plate, double lat, double lon, std::int64_t t) {
    Sighting s;
    s.kind = Kind::Vehicle;
    s.identifier = plate;
    s.label = "grey hatchback";
    s.latitude = lat;
    s.longitude = lon;
    s.time = t;
    return s;
}

// Six places a few km apart.
constexpr double kPlaces[6][2] = {{51.500, -0.120}, {51.520, -0.100}, {51.540, -0.140}, {51.480, -0.160}, {51.560, -0.080}, {51.460, -0.100}};

std::string tempPath() {
    char name[] = "/tmp/oliphauntXXXXXX";
    const int fd = mkstemp(name);
    if (fd >= 0) close(fd);
    std::remove(name);
    return name;
}

} // namespace

TEST(Oliphaunt, FollowerAcrossWeeksIsPersistent) {
    Memory m;
    Finding last;
    for (int d = 0; d < 10; ++d)
        for (int p = 0; p < 3; ++p) last = *m.observe(car("AB12 CDE", kPlaces[p][0], kPlaces[p][1], kT0 + d * kDay + p * 7200));
    EXPECT_EQ(last.concern, Concern::Persistent);
    EXPECT_EQ(last.places, 3);
    EXPECT_EQ(last.days, 10);
    EXPECT_EQ(last.sightings, 30);
    const auto report = m.report(Concern::Alert, kT0 + 10 * kDay);
    ASSERT_EQ(report.size(), 1u);
    EXPECT_NE(report[0].summary.find("3 places"), std::string::npos);
}

TEST(Oliphaunt, EscalatesWithEvidence) {
    Memory m;
    EXPECT_EQ(m.observe(car("X1", kPlaces[0][0], kPlaces[0][1], kT0))->concern, Concern::None);
    EXPECT_EQ(m.observe(car("X1", kPlaces[1][0], kPlaces[1][1], kT0 + kDay))->concern, Concern::Noted);
    EXPECT_EQ(m.observe(car("X1", kPlaces[2][0], kPlaces[2][1], kT0 + 2 * kDay))->concern, Concern::Notice);
    EXPECT_EQ(m.observe(car("X1", kPlaces[3][0], kPlaces[3][1], kT0 + 3 * kDay))->concern, Concern::Alert);
}

TEST(Oliphaunt, OnePlaceIsNotFollowing) {
    Memory m;
    Finding f;
    for (int d = 0; d < 30; ++d) f = *m.observe(car("NEIGHBOUR", kPlaces[0][0], kPlaces[0][1], kT0 + d * kDay));
    EXPECT_EQ(f.places, 1);
    EXPECT_LT(f.concern, Concern::Notice);
}

TEST(Oliphaunt, SameDayTrafficIsNotFollowing) {
    Memory m;
    Finding f;
    for (int p = 0; p < 4; ++p) f = *m.observe(car("TRAFFIC", kPlaces[p][0], kPlaces[p][1], kT0 + p * 3600));
    EXPECT_LT(f.concern, Concern::Notice);
}

TEST(Oliphaunt, SafePlacesAndTrustSuppress) {
    Memory m;
    m.addSafePlace({"home", kPlaces[0][0], kPlaces[0][1], 200});
    m.addSafePlace({"work", kPlaces[1][0], kPlaces[1][1], 200});
    Finding f;
    for (int d = 0; d < 5; ++d)
        for (int p = 0; p < 2; ++p) f = *m.observe(car("COLLEAGUE", kPlaces[p][0], kPlaces[p][1], kT0 + d * kDay + p * 3600));
    EXPECT_EQ(f.places, 0);
    EXPECT_LT(f.concern, Concern::Notice);

    for (int d = 0; d < 5; ++d)
        for (int p = 2; p < 6; ++p) m.observe(car("FRIEND", kPlaces[p][0], kPlaces[p][1], kT0 + d * kDay + p * 7200));
    EXPECT_GE(m.lookup(Kind::Vehicle, "FRIEND", kT0 + 6 * kDay)->concern, Concern::Alert);
    EXPECT_TRUE(m.trust(Kind::Vehicle, "friend"));
    EXPECT_EQ(m.lookup(Kind::Vehicle, "friend", kT0 + 6 * kDay)->concern, Concern::None);
    EXPECT_TRUE(m.lookup(Kind::Vehicle, "friend", kT0 + 6 * kDay)->trusted);
}

TEST(Oliphaunt, PlatesNormaliseAndAreNotStored) {
    const std::string path = tempPath();
    {
        Memory m(path);
        ASSERT_TRUE(m.observe(car("ab 12-cde", kPlaces[0][0], kPlaces[0][1], kT0)).has_value());
        EXPECT_TRUE(m.lookup(Kind::Vehicle, "AB12CDE").has_value());
    }
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_EQ(text.str().find("AB12"), std::string::npos);
    EXPECT_EQ(text.str().find("ab 12"), std::string::npos);
    std::remove(path.c_str());
}

TEST(Oliphaunt, MemorySurvivesRestartAndFileIsPrivate) {
    const std::string path = tempPath();
    {
        Memory m(path);
        m.addSafePlace({"home", kPlaces[0][0], kPlaces[0][1], 200});
        for (int d = 0; d < 4; ++d) m.observe(car("KEEP1", kPlaces[1 + d % 3][0], kPlaces[1 + d % 3][1], kT0 + d * kDay));
        m.trust(Kind::Vehicle, "MINE");
    }
    Memory again(path);
    EXPECT_EQ(again.entityCount(), 2u);
    EXPECT_EQ(again.safePlaces().size(), 1u);
    const auto f = again.lookup(Kind::Vehicle, "keep1", kT0 + 4 * kDay);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->sightings, 4);
    EXPECT_TRUE(again.lookup(Kind::Vehicle, "MINE")->trusted);
    EXPECT_EQ(access(path.c_str(), R_OK | W_OK), 0);
    std::remove(path.c_str());
}

TEST(Oliphaunt, ForgetAndRetention) {
    Options o;
    o.retentionDays = 30;
    Memory m({}, o);
    const std::int64_t now = std::time(nullptr);
    std::string err;
    EXPECT_FALSE(m.observe(car("OLD", kPlaces[0][0], kPlaces[0][1], now - 60 * kDay), &err).has_value());
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(m.lookup(Kind::Vehicle, "OLD").has_value());
    m.observe(car("AGING", kPlaces[0][0], kPlaces[0][1], now - 29 * kDay));
    EXPECT_EQ(m.prune(now + 5 * kDay), 1u);
    EXPECT_EQ(m.entityCount(), 0u);
    m.observe(car("NEW", kPlaces[0][0], kPlaces[0][1], now - kDay));
    EXPECT_TRUE(m.lookup(Kind::Vehicle, "NEW").has_value());
    EXPECT_TRUE(m.forget(Kind::Vehicle, "new"));
    EXPECT_FALSE(m.forget(Kind::Vehicle, "new"));
    m.observe(car("A", kPlaces[0][0], kPlaces[0][1], now));
    m.forgetAll();
    EXPECT_EQ(m.entityCount(), 0u);
}

TEST(Oliphaunt, TrustAndForgetByIdPrefix) {
    Memory m;
    const auto f = *m.observe(car("ID1", kPlaces[0][0], kPlaces[0][1], kT0));
    EXPECT_FALSE(m.trustById(f.id.substr(0, 3)));
    EXPECT_FALSE(m.trustById("ffffffffffff"));
    EXPECT_TRUE(m.trustById(f.id));
    EXPECT_TRUE(m.lookup(Kind::Vehicle, "ID1")->trusted);
    EXPECT_TRUE(m.forgetById(f.id.substr(0, 8)));
    EXPECT_EQ(m.entityCount(), 0u);
}

TEST(Oliphaunt, RejectsBadSightings) {
    Memory m;
    std::string err;
    Sighting s = car("", 1, 1, kT0);
    EXPECT_FALSE(m.observe(s, &err).has_value());
    s = car("A1", 91, 0, kT0);
    EXPECT_FALSE(m.observe(s, &err).has_value());
    s = car("A1", 10, 10, kT0);
    s.longitude.reset();
    EXPECT_FALSE(m.observe(s, &err).has_value());
    EXPECT_EQ(m.entityCount(), 0u);
}

TEST(Oliphaunt, NamedPlacesWorkWithoutAFix) {
    Memory m;
    Finding f;
    const char* names[] = {"Elm St", "Station", "Library"};
    for (int d = 0; d < 3; ++d) {
        Sighting s;
        s.kind = Kind::Device;
        s.identifier = "AA:BB:CC:DD:EE:FF";
        s.place = names[d];
        s.time = kT0 + d * kDay;
        f = *m.observe(s);
    }
    EXPECT_EQ(f.places, 3);
    EXPECT_GE(f.concern, Concern::Notice);
}

TEST(Oliphaunt, CorruptFileIsKeptNotOverwritten) {
    const std::string path = tempPath();
    {
        std::ofstream out(path);
        out << "{not json";
    }
    Memory m(path);
    EXPECT_FALSE(m.lastError().empty());
    EXPECT_EQ(access((path + ".corrupt").c_str(), F_OK), 0);
    std::remove((path + ".corrupt").c_str());
    std::remove(path.c_str());
}

#include <gygax/core/log.hpp>
#include <gygax/net/http.hpp>
#include <gygax/service/service.hpp>

TEST(OliphauntService, ToolsRememberAndReport) {
    using namespace gygax;
    log::setLevel(log::Level::Error);
    service::ServiceConfig c;
    c.port = 0;
    c.token = "tok";
    c.engines = {"echo"};
    service::Service svc(c);
    std::string err;
    ASSERT_TRUE(svc.start(&err)) << err;
    const std::string base = "http://127.0.0.1:" + std::to_string(svc.port());
    auto call = [&](const std::string& tool, const std::string& input) {
        json::Value body = json::Value::object();
        body["input"] = input;
        return net::httpRequest(*net::Url::parse(base + "/v1/tools/" + tool + "/invoke"), "POST", body.dump(),
                                {{"Authorization", "Bearer tok"}, {"Content-Type", "application/json"}});
    };
    const std::int64_t t0 = (std::time(nullptr) / kDay - 10) * kDay + 3600;
    for (int d = 0; d < 8; ++d)
        for (int p = 0; p < 3; ++p) {
            const std::string in = "{\"identifier\":\"AB12 CDE\",\"latitude\":" + std::to_string(kPlaces[p][0]) +
                                   ",\"longitude\":" + std::to_string(kPlaces[p][1]) + ",\"time\":" +
                                   std::to_string(t0 + d * kDay + p * 7200) + "}";
            ASSERT_EQ(call("oliphaunt.observe", in).status, 200);
        }
    const auto report = call("oliphaunt.report", "{\"minimum\":\"alert\"}");
    ASSERT_EQ(report.status, 200);
    EXPECT_NE(report.body.find("persistent"), std::string::npos);
    EXPECT_EQ(call("oliphaunt.observe", "{\"identifier\":\"\"}").status, 422);
    EXPECT_EQ(call("oliphaunt.forget", "{\"all\":true}").status, 200);
    EXPECT_NE(call("oliphaunt.report", "{}").body.find("\\\"entities\\\":0"), std::string::npos);
}
