#include <gtest/gtest.h>

#include <cmath>

#include <glyder/crypto.hpp>
#include <glyder/draw.hpp>
#include <glyder/handling.hpp>
#include <glyder/image.hpp>
#include <glyder/kit.hpp>
#include <glyder/math.hpp>
#include <glyder/net.hpp>
#include <glyder/wire.hpp>

using namespace glyder;

namespace {
std::string hexOf(std::span<const std::uint8_t> d) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (auto b : d) {
        s += h[b >> 4];
        s += h[b & 15];
    }
    return s;
}
std::vector<std::uint8_t> bytesOf(std::string_view s) {
    return {s.begin(), s.end()};
}
} // namespace

TEST(GlyderCrypto, Sha256MatchesNistVectors) {
    EXPECT_EQ(hexOf(crypto::sha256(bytesOf(""))), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hexOf(crypto::sha256(bytesOf("abc"))), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hexOf(crypto::sha256(bytesOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::vector<std::uint8_t> million(1000000, 'a');
    EXPECT_EQ(hexOf(crypto::sha256(million)), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(GlyderCrypto, HmacMatchesRfc4231) {
    std::vector<std::uint8_t> key1(20, 0x0b);
    EXPECT_EQ(hexOf(crypto::hmac(key1, bytesOf("Hi There"))), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    EXPECT_EQ(hexOf(crypto::hmac(bytesOf("Jefe"), bytesOf("what do ya want for nothing?"))),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    std::vector<std::uint8_t> key6(131, 0xaa);
    EXPECT_EQ(hexOf(crypto::hmac(key6, bytesOf("Test Using Larger Than Block-Size Key - Hash Key First"))),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(GlyderMath, AttitudeFollowsCompassConvention) {
    const Vec3 north = rotate(attitude(0), {1, 0, 0});
    EXPECT_NEAR(north.y, 1.0f, 1e-5f);
    const Vec3 east = rotate(attitude(kPi / 2), {1, 0, 0});
    EXPECT_NEAR(east.x, 1.0f, 1e-5f);
    const Vec3 up = rotate(attitude(0, 0.3f), {1, 0, 0});
    EXPECT_NEAR(up.z, std::sin(0.3f), 1e-5f);
    const Vec3 rightWing = rotate(attitude(0, 0, 0.4f), {0, -1, 0});
    EXPECT_LT(rightWing.z, -0.3f); // positive roll: right wing down
}

TEST(GlyderMath, RenderFrameAndInverse) {
    const Vec3 p{3, 4, 5};
    EXPECT_NEAR(fromRender(toRender(p)).y, 4.0f, 1e-6f);
    const Mat4 w = worldToRender();
    const Vec3 r = w.transformPoint(p);
    EXPECT_FLOAT_EQ(r.x, 3);
    EXPECT_FLOAT_EQ(r.y, 5);
    EXPECT_FLOAT_EQ(r.z, -4);
    const Mat4 m = compose({1, 2, 3}, axisAngle({0.3f, 1, 0.2f}, 0.7f), {2, 2, 2});
    const Mat4 i = inverse(m) * m;
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) EXPECT_NEAR(i.m[c][rr], c == rr ? 1.0f : 0.0f, 1e-5f);
}

TEST(GlyderMath, ReversedInfiniteProjection) {
    const Mat4 p = perspective(1.0f, 1.0f, 0.1f);
    const Vec4 nearPt = p * Vec4{0, 0, -0.1f, 1};
    const Vec4 farPt = p * Vec4{0, 0, -1e6f, 1};
    EXPECT_NEAR(nearPt.z / nearPt.w, 1.0f, 1e-5f);
    EXPECT_LT(farPt.z / farPt.w, 1e-6f);
    const Vec4 above = p * Vec4{0, 1, -2, 1};
    EXPECT_LT(above.y / above.w, 0.0f); // Vulkan: up is negative y
}

TEST(GlyderKit, EveryShapeIsAValidClosedLowPolyModel) {
    for (kit::Shape s : kit::allShapes()) {
        SCOPED_TRACE(std::string(kit::name(s)));
        const kit::Mesh m = kit::build(s);
        EXPECT_GT(m.triangles(), 8u);
        EXPECT_LT(m.triangles(), 4000u) << "low poly";
        EXPECT_EQ(m.indices.size(), m.vertices.size());
        for (const auto& v : m.vertices) {
            ASSERT_TRUE(std::isfinite(v.px) && std::isfinite(v.py) && std::isfinite(v.pz));
            const int n2 = v.nx * v.nx + v.ny * v.ny + v.nz * v.nz;
            EXPECT_NEAR(std::sqrt(static_cast<double>(n2)), 127.0, 3.0);
            EXPECT_LE(v.a / 64, 3);
        }
        const Vec3 size = m.bounds.size();
        EXPECT_GT(std::max(size.x, std::max(size.y, size.z)), 0.1f);
        EXPECT_LT(std::max(size.x, std::max(size.y, size.z)), 40.0f);
        EXPECT_EQ(kit::shapeFromName(kit::name(s)), s);
    }
}

TEST(GlyderKit, PrimitivesFacePointOutward) {
    // Every primitive the kit is built from orients its faces away from its own solid, mirrored or
    // not, so back-face culling is right for every model.
    auto check = [](const char* what, auto&& build, auto&& centreAt) {
        for (bool mirror : {false, true}) {
            kit::Builder b;
            if (mirror) b.push(scaling({1, -1, 1}));
            build(b);
            const kit::Mesh m = b.finish();
            ASSERT_GT(m.vertices.size(), 0u) << what;
            for (std::size_t i = 0; i < m.vertices.size(); i += 3) {
                const auto &v0 = m.vertices[i], &v1 = m.vertices[i + 1], &v2 = m.vertices[i + 2];
                const Vec3 p{(v0.px + v1.px + v2.px) / 3, (v0.py + v1.py + v2.py) / 3, (v0.pz + v1.pz + v2.pz) / 3};
                const Vec3 n{static_cast<float>(v0.nx), static_cast<float>(v0.ny), static_cast<float>(v0.nz)};
                Vec3 c = centreAt(Vec3{p.x, mirror ? -p.y : p.y, p.z});
                if (mirror) c.y = -c.y;
                // The geometric winding agrees with the stored normal...
                const Vec3 g = cross(Vec3{v1.px - v0.px, v1.py - v0.py, v1.pz - v0.pz}, Vec3{v2.px - v0.px, v2.py - v0.py, v2.pz - v0.pz});
                EXPECT_GT(dot(g, n), 0.0f) << what;
                // ...and points away from the solid.
                EXPECT_GT(dot(n, p - c), 0.0f) << what << (mirror ? " mirrored" : "");
            }
        }
    };
    check("box", [](kit::Builder& b) { b.box({1, 2, 3}, {2, 1, 0.5f}); }, [](Vec3) { return Vec3{1, 2, 3}; });
    check("wedge", [](kit::Builder& b) { b.wedgeBox({0, 1, 0}, {2, 1, 1}, 0.4f, 0.2f, 0.1f); }, [](Vec3) { return Vec3{0, 1, 0}; });
    check("sphere", [](kit::Builder& b) { b.icosphere({0, 2, 1}, 1.0f, 2); }, [](Vec3) { return Vec3{0, 2, 1}; });
    check("cylinder", [](kit::Builder& b) { b.cylinder({0, 1, 0}, {0, 1, 2}, 0.5f, 8); }, [](Vec3) { return Vec3{0, 1, 1}; });
    check(
        "lathe", [](kit::Builder& b) { b.lathe(std::array<Vec2, 3>{{{1, 0.3f}, {0, 0.5f}, {-1, 0.2f}}}, 10); },
        [](Vec3) { return Vec3{0, 0, 0}; });
    // A swept, tapered wing has no single interior point: use the mid-chord point at each span station.
    check(
        "wing", [](kit::Builder& b) { b.wing({0.5f, 1, 0}, 1.0f, {0.0f, 3, 0.1f}, 0.5f, 0.12f); },
        [](Vec3 p) {
            const float t = clamp((p.y - 1.0f) / 2.0f, 0.0f, 1.0f);
            return lerp(Vec3{0.0f, 1, 0.0f}, Vec3{-0.25f, 3, 0.1f}, t);
        });
}

TEST(GlyderKit, ExportsValidGlb) {
    const auto glb = kit::exportGlb();
    ASSERT_GT(glb.size(), 1000u);
    EXPECT_EQ(glb[0], 'g');
    EXPECT_EQ(glb[1], 'l');
    EXPECT_EQ(glb[2], 'T');
    EXPECT_EQ(glb[3], 'F');
    const std::uint32_t total = glb[8] | glb[9] << 8 | glb[10] << 16 | static_cast<std::uint32_t>(glb[11]) << 24;
    EXPECT_EQ(total, glb.size());
    const std::string json(reinterpret_cast<const char*>(glb.data() + 20), glb[12] | glb[13] << 8 | glb[14] << 16);
    EXPECT_NE(json.find("\"name\":\"kite\""), std::string::npos);
    EXPECT_NE(json.find("KHR_materials_unlit"), std::string::npos);
}

TEST(GlyderWire, QuaternionPacksWithinAFractionOfADegree) {
    for (int i = 0; i < 500; ++i) {
        const Quat q = normalize(Quat{std::sin(static_cast<float>(i) * 1.3f), std::cos(static_cast<float>(i) * 0.7f),
                                      std::sin(static_cast<float>(i) * 2.1f + 1), std::cos(static_cast<float>(i) * 0.37f)});
        const Quat r = wire::unpackQuat(wire::packQuat(q));
        const float d = std::fabs(q.x * r.x + q.y * r.y + q.z * r.z + q.w * r.w);
        EXPECT_GT(d, std::cos(0.25f * kDeg));
    }
}

TEST(GlyderWire, MessagesRoundTrip) {
    wire::Snapshot s;
    s.hostTime = 1234.5;
    s.rate = 4;
    s.tick = 77;
    s.parts = 2;
    s.part = 1;
    wire::EntityState e;
    e.id = 0xDEADBEEF;
    e.shape = static_cast<std::uint16_t>(kit::Shape::Fighter);
    e.team = 2;
    e.flags = wire::kAlive | wire::kDrivable;
    e.pos = {1234.5f, -987.25f, 4500.0f};
    e.vel = {250.0f, -0.5f, 7600.0f};
    e.q = attitude(1.0f, 0.2f, -0.3f);
    e.tint = {10, 20, 30};
    e.scale = 2.0f;
    s.entities = {e, e};
    wire::Writer w;
    wire::encode(w, s);
    EXPECT_EQ(w.size(), wire::kSnapshotOverhead + 2 * wire::kEntityBytes);
    wire::Reader r(w.data());
    const auto back = wire::decode<wire::Snapshot>(r);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->tick, 77u);
    EXPECT_FLOAT_EQ(back->rate, 4);
    ASSERT_EQ(back->entities.size(), 2u);
    const auto& b = back->entities[1];
    EXPECT_EQ(b.id, e.id);
    EXPECT_EQ(b.shape, e.shape);
    EXPECT_FLOAT_EQ(b.pos.y, -987.25f);
    EXPECT_NEAR(b.vel.x, 250.0f, 0.25f);
    EXPECT_NEAR(b.vel.y, -0.5f, 0.001f);
    EXPECT_NEAR(b.vel.z, 7600.0f, 4.0f);
    EXPECT_NEAR(b.scale, 2.0f, 0.05f);
    EXPECT_EQ(b.tint.g, 20);

    wire::DropState d{42, wire::Drop::Control, 1.5f, "you have control", 1, 18, 40, 1.2f, 5, 6};
    wire::Writer w2;
    wire::encode(w2, d);
    wire::Reader r2(w2.data());
    const auto d2 = wire::decode<wire::DropState>(r2);
    ASSERT_TRUE(d2);
    EXPECT_EQ(d2->state, wire::Drop::Control);
    EXPECT_EQ(d2->reason, "you have control");
    EXPECT_FLOAT_EQ(d2->maxSpeed, 40);

    wire::Control c;
    c.entity = 9;
    c.axes = {1, -1, 0.5f, -0.25f, 0, 0.75f};
    c.lookYaw = 0.5f;
    wire::Writer w3;
    wire::encode(w3, c);
    wire::Reader r3(w3.data());
    const auto c2 = wire::decode<wire::Control>(r3);
    ASSERT_TRUE(c2);
    EXPECT_NEAR(c2->axes[2], 0.5f, 1e-4f);
    EXPECT_NEAR(c2->axes[1], -1.0f, 1e-4f);
    EXPECT_NEAR(c2->lookYaw, 0.5f, 1e-3f);

    // Truncated input is rejected, not misread.
    wire::Reader shortReader(std::span(w.data()).first(30));
    EXPECT_FALSE(wire::decode<wire::Snapshot>(shortReader));
}

TEST(GlyderWire, PacketsAreAuthenticated) {
    const auto key = wire::pairingKey("123456");
    auto p = net::packet(wire::Type::Order, 7, 3, wire::kReliable, wire::Order{"posture wide"}, key);
    EXPECT_LE(p.size(), wire::kMaxPacket);
    EXPECT_TRUE(wire::verify(p, key));
    EXPECT_FALSE(wire::verify(p, wire::pairingKey("123457")));
    p[20] ^= 1;
    EXPECT_FALSE(wire::verify(p, key));
}

TEST(GlyderWire, HeightfieldRoundTripsToACentimetre) {
    wire::Heightfield f;
    f.w = 33;
    f.h = 17;
    f.cell = 50;
    f.x0 = -800;
    f.y0 = -400;
    for (int j = 0; j < f.h; ++j)
        for (int i = 0; i < f.w; ++i)
            f.z.push_back(310.0f + 40.0f * std::sin(static_cast<float>(i) * 0.3f) * std::cos(static_cast<float>(j) * 0.2f));
    const auto back = wire::Heightfield::decode(f.encode());
    ASSERT_TRUE(back);
    for (std::size_t k = 0; k < f.z.size(); ++k) EXPECT_NEAR(back->z[k], f.z[k], 0.01f);
    EXPECT_NEAR(back->sample(-800 + 25, -400), (f.at(0, 0) + f.at(1, 0)) / 2, 0.01f);
}

TEST(GlyderNet, ReliableChannelDeliversInOrderThroughLoss) {
    net::ReliableIn in;
    // Arrivals: 2, 4, 1 (now 1 and 2 deliver), 4 again (duplicate), 3 (3 and 4 deliver).
    const std::uint8_t b = 0;
    EXPECT_TRUE(in.accept(2, wire::Type::Event, std::span(&b, 1)).empty());
    EXPECT_TRUE(in.accept(4, wire::Type::Event, std::span(&b, 1)).empty());
    EXPECT_EQ(in.ack().cumulative, 0u);
    EXPECT_EQ(in.ack().mask, 0b1010u);
    EXPECT_EQ(in.accept(1, wire::Type::Event, std::span(&b, 1)).size(), 2u);
    EXPECT_TRUE(in.accept(4, wire::Type::Event, std::span(&b, 1)).empty());
    EXPECT_EQ(in.accept(3, wire::Type::Event, std::span(&b, 1)).size(), 2u);
    EXPECT_EQ(in.ack().cumulative, 4u);

    net::ReliableOut out;
    for (int i = 0; i < 3; ++i) out.sent(out.nextSeq(), {1, 2, 3}, 0.0);
    EXPECT_EQ(out.inFlight(), 3u);
    out.acked({1, 0b10}, 0.02); // 1 and 3 arrived
    EXPECT_EQ(out.inFlight(), 1u);
    EXPECT_TRUE(out.due(0.03).empty());
    EXPECT_EQ(out.due(1.0).size(), 1u);
}

TEST(GlyderHandling, AircraftKeepFlyingAndGroundVehiclesStayDown) {
    Handling air{Handling::Model::Air, 15, 40, 0.8f, 6, 5};
    Motion m;
    m.pos = {0, 0, 300};
    m.speed = 20;
    const float idle[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 100; ++i) step(m, air, idle, 0.05f, 0);
    EXPECT_NEAR(m.pos.y, 100.0f, 1.0f); // north at 20 m/s for 5 s
    const float slow[6] = {-1, 0, 0, 0, -1, 0};
    for (int i = 0; i < 400; ++i) step(m, air, slow, 0.05f, 250);
    EXPECT_GE(m.speed, 15.0f);
    EXPECT_GE(m.pos.z, 252.0f); // never flown into the ground
    const float right[6] = {0, 0, 0, 1, 0, 0};
    const float h0 = m.heading;
    step(m, air, right, 0.5f, 0);
    EXPECT_GT(m.heading, h0);
    EXPECT_LT(rotate(m.attitude(Handling::Model::Air), {0, -1, 0}).z, 0.0f); // banked right into the turn

    Handling ground{Handling::Model::Ground, 0, 15, 0.6f, 0, 4};
    Motion g;
    g.pos = {0, 0, 5};
    const float go[6] = {1, 0, 0, 0, 0, 0};
    for (int i = 0; i < 200; ++i) step(g, ground, go, 0.05f, 12);
    EXPECT_FLOAT_EQ(g.pos.z, 12.0f);
    EXPECT_NEAR(g.speed, 15.0f, 0.01f);
    for (int i = 0; i < 200; ++i) hold(g, ground, 0.05f, 12);
    EXPECT_NEAR(g.speed, 0.0f, 0.01f);
}

TEST(GlyderDraw, StrokeFontCoversItsCharacterSet) {
    std::vector<render::Line> lines;
    render::text(lines, "GYGAX 0.4.0 MATRIX: DROP-IN [OK] 100%", {}, {1, 0, 0}, {0, 1, 0}, 0.12f, {255, 255, 255});
    EXPECT_GT(lines.size(), 60u);
    for (const auto& l : lines) EXPECT_LT(length(l.b - l.a), 0.2f);
    EXPECT_NEAR(render::textWidth("AB", 12), 19.0f, 1e-4f);
    std::vector<render::Line> centred;
    render::text(centred, "AB", {}, {1, 0, 0}, {0, 1, 0}, 12, {255, 255, 255}, render::Align::Centre);
    float lo = 1e9f, hi = -1e9f;
    for (const auto& l : centred) {
        lo = std::min({lo, l.a.x, l.b.x});
        hi = std::max({hi, l.a.x, l.b.x});
    }
    EXPECT_NEAR(lo + hi, 0.0f, 1e-3f);
}

TEST(GlyderImage, PngHasValidChunks) {
    std::vector<std::uint8_t> px(4 * 3 * 2, 200);
    const auto png = image::encodePng(3, 2, px);
    ASSERT_GT(png.size(), 50u);
    EXPECT_EQ(png[1], 'P');
    // The IHDR chunk's CRC covers its type and data.
    const std::uint32_t crc = image::crc32(std::span(png).subspan(12, 17));
    EXPECT_EQ(png[29], static_cast<std::uint8_t>(crc >> 24));
    EXPECT_EQ(png[32], static_cast<std::uint8_t>(crc));
}
