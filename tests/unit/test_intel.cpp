#include <gtest/gtest.h>

#include <gygax/intel/metadata.hpp>
#include <gygax/intel/osint.hpp>
#include <gygax/core/json.hpp>

#include <cstdint>
#include <vector>

using namespace gygax;
using namespace gygax::intel;

namespace {

ImageMetadata eiffelPhoto() {
    ImageMetadata m;
    m.description = "Eiffel Tower at dusk";
    m.make = "Acme";
    m.model = "Cam1";
    m.software = "firmware 1.2";
    m.dateTime = "2025:06:01 12:00:00";
    m.latitude = 48.8584;
    m.longitude = 2.2945;
    m.altitude = 35.0;
    m.heading = 90.0;
    return m;
}

constexpr std::int64_t kNow = 1750000000;

Event makeEvent(const char* source, double lat, double lon, int severity, Level level = Level::Local,
                std::int64_t age = 600, double confidence = 0.6) {
    Event e;
    e.title = "event";
    e.source = source;
    e.latitude = lat;
    e.longitude = lon;
    e.severity = severity;
    e.level = level;
    e.domain = Domain::Infrastructure;
    e.time = kNow - age;
    e.confidence = confidence;
    return e;
}

} // namespace

TEST(IntelMetadata, TiffRoundTrip) {
    ImageMetadata out;
    ASSERT_TRUE(parseTiff(buildTiff(eiffelPhoto()), out));
    EXPECT_EQ(out.description, "Eiffel Tower at dusk");
    EXPECT_EQ(out.make, "Acme");
    ASSERT_TRUE(out.hasGps());
    EXPECT_NEAR(*out.latitude, 48.8584, 1e-4);
    EXPECT_NEAR(*out.longitude, 2.2945, 1e-4);
    EXPECT_NEAR(*out.heading, 90.0, 1e-3);
}

TEST(IntelMetadata, JpegRoundTripAndSouthWest) {
    ImageMetadata in = eiffelPhoto();
    in.latitude = -33.8568;
    in.longitude = -70.5;
    ImageMetadata out;
    ASSERT_TRUE(parseImage(buildJpeg(in), out));
    EXPECT_EQ(out.format, "jpeg");
    EXPECT_NEAR(*out.latitude, -33.8568, 1e-4);
    EXPECT_NEAR(*out.longitude, -70.5, 1e-4);
}

TEST(IntelMetadata, HostileInputIsRejectedSafely) {
    ImageMetadata out;
    EXPECT_FALSE(parseImage({}, out));
    std::vector<std::uint8_t> junk(64, 0xFF);
    EXPECT_FALSE(parseImage(junk, out));
    auto jpeg = buildJpeg(eiffelPhoto());
    for (std::size_t n = 0; n < jpeg.size(); n += 3) {
        ImageMetadata partial;
        (void)parseImage(std::span<const std::uint8_t>(jpeg.data(), n), partial); // must not crash
    }
    auto tiff = buildTiff(eiffelPhoto());
    for (std::size_t i = 8; i < tiff.size(); ++i) {
        auto bad = tiff;
        bad[i] = 0xFF;
        ImageMetadata o;
        (void)parseTiff(bad, o);
    }
}

TEST(IntelOsint, GeoMath) {
    EXPECT_NEAR(haversineMeters(0, 0, 0, 1), 111195.0, 200.0);
    EXPECT_DOUBLE_EQ(haversineMeters(10, 10, 10, 10), 0.0);
    auto t = parseIsoTime("2025-01-01T00:00:00Z");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, 1735689600);
    EXPECT_FALSE(parseIsoTime("garbage").has_value());
}

TEST(IntelOsint, LoadsEventsAndGeoJson) {
    OsintEngine engine;
    auto feed = json::parse(R"({"events":[{"title":"a","latitude":1,"longitude":2,"severity":3,"level":"regional"},{"title":"bad"}]})");
    ASSERT_TRUE(feed.has_value());
    std::size_t skipped = 0;
    EXPECT_EQ(engine.loadJson(*feed, nullptr, &skipped), 1u);
    EXPECT_EQ(skipped, 1u);

    auto geo = json::parse(R"({"type":"FeatureCollection","features":[{"type":"Feature",
        "properties":{"mag":5.1,"place":"somewhere","time":1750000000000},
        "geometry":{"type":"Point","coordinates":[10.0,20.0,5.0]}}]})");
    ASSERT_TRUE(geo.has_value());
    EXPECT_EQ(engine.loadJson(*geo), 1u);
    EXPECT_EQ(engine.eventCount(), 2u);

    std::string err;
    auto notFeed = json::parse("42");
    EXPECT_EQ(engine.loadJson(*notFeed, &err), 0u);
    EXPECT_FALSE(err.empty());
}

TEST(IntelOsint, LevelBubbling) {
    OsintEngine engine;
    engine.addEvent(makeEvent("a", 1, 1, 1, Level::Local));
    engine.addEvent(makeEvent("b", 2, 2, 5, Level::Local));
    EXPECT_EQ(engine.feed(Level::Local).size(), 2u);
    const auto global = engine.feed(Level::Global);
    ASSERT_EQ(global.size(), 1u);
    EXPECT_EQ(global[0].severity, 5);
}

TEST(IntelOsint, CorroborationRaisesConfidence) {
    OsintEngine engine;
    Event a = makeEvent("alpha", 10, 10, 3);
    Event b = makeEvent("beta", 10.01, 10.01, 3);
    engine.addEvent(a);
    EXPECT_NEAR(engine.confidenceOf(a), 0.6, 1e-9);
    engine.addEvent(b);
    EXPECT_NEAR(engine.confidenceOf(a), 1 - 0.4 * 0.4, 1e-9);
    Event far = makeEvent("gamma", 50, 50, 3);
    engine.addEvent(far);
    EXPECT_NEAR(engine.confidenceOf(far), 0.6, 1e-9);
}

TEST(IntelOsint, AssessAndReconcile) {
    OsintEngine engine;
    EXPECT_EQ(engine.assess(5, 5, 10, kNow).threat, Threat::Clear);
    engine.addEvent(makeEvent("a", 5, 5, 5, Level::Local, 300, 0.9));
    const auto a = engine.assess(5, 5, 10, kNow);
    EXPECT_EQ(a.threat, Threat::Critical);
    EXPECT_EQ(a.events.size(), 1u);

    Report secure;
    secure.latitude = 5;
    secure.longitude = 5;
    secure.claimsSecure = true;
    EXPECT_TRUE(engine.reconcile(secure, kNow).discrepancy);

    Report calm = secure;
    calm.latitude = 60;
    calm.longitude = 60;
    EXPECT_FALSE(engine.reconcile(calm, kNow).discrepancy);

    Report hostile = calm;
    hostile.claimsSecure = false;
    EXPECT_TRUE(engine.reconcile(hostile, kNow).discrepancy);
}

TEST(IntelOsint, OldEventsDecay) {
    OsintEngine engine;
    engine.addEvent(makeEvent("a", 5, 5, 4, Level::Local, 10 * 24 * 3600, 0.9));
    EXPECT_EQ(engine.assess(5, 5, 10, kNow).threat, Threat::Clear);
}

TEST(IntelGeolocation, VerifiesMatchingPhoto) {
    OsintEngine engine;
    const auto m = engine.verifyLocation(eiffelPhoto());
    EXPECT_TRUE(m.verified);
    EXPECT_GE(m.confidence, 0.8);
    EXPECT_NE(m.locationName.find("Eiffel"), std::string::npos);
}

TEST(IntelGeolocation, FlagsSpoofedAndEditedPhotos) {
    OsintEngine engine;
    ImageMetadata spoof = eiffelPhoto();
    spoof.description = "Statue of Liberty";
    const auto s = engine.verifyLocation(spoof);
    EXPECT_FALSE(s.verified);
    EXPECT_FALSE(s.verificationNotes.empty());

    ImageMetadata edited = eiffelPhoto();
    edited.software = "Adobe Photoshop 25";
    EXPECT_LT(engine.verifyLocation(edited).confidence, engine.verifyLocation(eiffelPhoto()).confidence);

    ImageMetadata nogps;
    EXPECT_FALSE(engine.verifyLocation(nogps).verified);
}

TEST(IntelGeolocation, CustomSiteAndOverlay) {
    OsintEngine engine;
    engine.addSite({"Test Pier", 54.3, 6.8, 200.0});
    ImageMetadata m;
    m.latitude = 54.3001;
    m.longitude = 6.8001;
    EXPECT_TRUE(engine.verifyLocation(m).verified);

    EXPECT_TRUE(engine.isOverlayActive());
    EXPECT_FALSE(engine.toggleOverlay());
    EXPECT_TRUE(engine.toggleOverlay());
    const Level before = engine.overlayLevel();
    EXPECT_NE(engine.cycleLevel(), before);
}
