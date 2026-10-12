#include <glyder/kit.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace glyder::kit {

namespace {

constexpr float F(int v) {
    return static_cast<float>(v);
}

std::int8_t snorm8(float v) {
    return static_cast<std::int8_t>(std::lround(clamp(v, -1.0f, 1.0f) * 127.0f));
}

Vec3 centroid(std::span<const Vec3> pts) {
    Vec3 c;
    for (const Vec3& p : pts) c += p;
    return pts.empty() ? c : c / F(static_cast<int>(pts.size()));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Builder

void Builder::push(const Mat4& m) {
    stack_.push_back(stack_.back() * m);
}

void Builder::pop() {
    if (stack_.size() > 1) stack_.pop_back();
}

void Builder::emit(Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 n = cross(b - a, c - a);
    const float area2 = length(n);
    if (area2 < 1e-10f) return;
    const Vec3 u = n / area2;
    const auto code = static_cast<std::uint8_t>(static_cast<unsigned>(material_) * 64u);
    for (const Vec3& p : {a, b, c})
        v_.push_back({p.x, p.y, p.z, snorm8(u.x), snorm8(u.y), snorm8(u.z), 0, colour_.r, colour_.g, colour_.b, code});
}

void Builder::tri(Vec3 a, Vec3 b, Vec3 c, Vec3 inside) {
    const Mat4& m = stack_.back();
    a = m.transformPoint(a);
    b = m.transformPoint(b);
    c = m.transformPoint(c);
    inside = m.transformPoint(inside);
    const Vec3 n = cross(b - a, c - a);
    if (dot(n, (a + b + c) / 3.0f - inside) < 0) std::swap(b, c);
    emit(a, b, c);
}

void Builder::quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 inside) {
    tri(a, b, c, inside);
    tri(a, c, d, inside);
}

void Builder::panel(Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    const Mat4& m = stack_.back();
    a = m.transformPoint(a);
    b = m.transformPoint(b);
    c = m.transformPoint(c);
    d = m.transformPoint(d);
    emit(a, b, c);
    emit(a, c, d);
    emit(a, c, b);
    emit(a, d, c);
}

void Builder::box(Vec3 c, Vec3 s) {
    wedgeBox(c, s, 0, 0, 0);
}

void Builder::wedgeBox(Vec3 c, Vec3 s, float insetX, float insetY, float shiftX) {
    const Vec3 h = s * 0.5f;
    const float tx = h.x - insetX, ty = h.y - insetY;
    const Vec3 b0{c.x - h.x, c.y - h.y, c.z - h.z}, b1{c.x + h.x, c.y - h.y, c.z - h.z}, b2{c.x + h.x, c.y + h.y, c.z - h.z},
        b3{c.x - h.x, c.y + h.y, c.z - h.z};
    const Vec3 t0{c.x + shiftX - tx, c.y - ty, c.z + h.z}, t1{c.x + shiftX + tx, c.y - ty, c.z + h.z},
        t2{c.x + shiftX + tx, c.y + ty, c.z + h.z}, t3{c.x + shiftX - tx, c.y + ty, c.z + h.z};
    quad(b0, b1, b2, b3, c);
    quad(t0, t1, t2, t3, c);
    quad(b0, b1, t1, t0, c);
    quad(b1, b2, t2, t1, c);
    quad(b2, b3, t3, t2, c);
    quad(b3, b0, t0, t3, c);
}

void Builder::loft(std::span<const std::vector<Vec3>> rings, bool capStart, bool capEnd) {
    if (rings.size() < 2) return;
    const std::size_t n = rings.front().size();
    for (const auto& r : rings)
        if (r.size() != n || n < 3) return; // a modelling error; the kit tests catch it
    for (std::size_t i = 0; i + 1 < rings.size(); ++i) {
        const auto& r0 = rings[i];
        const auto& r1 = rings[i + 1];
        const Vec3 inside = (centroid(r0) + centroid(r1)) * 0.5f;
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t k1 = (k + 1) % n;
            tri(r0[k], r0[k1], r1[k1], inside);
            tri(r0[k], r1[k1], r1[k], inside);
        }
    }
    auto cap = [&](const std::vector<Vec3>& r, const std::vector<Vec3>& neighbour) {
        const Vec3 c = centroid(r);
        const Vec3 inside = centroid(neighbour);
        for (std::size_t k = 0; k < n; ++k) tri(c, r[k], r[(k + 1) % n], inside);
    };
    if (capStart) cap(rings.front(), rings[1]);
    if (capEnd) cap(rings.back(), rings[rings.size() - 2]);
}

void Builder::lathe(std::span<const Vec2> profile, int segments, float rollOffset) {
    std::vector<std::vector<Vec3>> rings;
    for (const Vec2& p : profile) {
        std::vector<Vec3> r;
        for (int k = 0; k < segments; ++k) {
            const float a = 2 * kPi * F(k) / F(segments) + rollOffset;
            r.push_back({p.x, p.y * std::cos(a), p.y * std::sin(a)});
        }
        rings.push_back(std::move(r));
    }
    loft(rings, profile.front().y > 1e-6f, profile.back().y > 1e-6f);
}

namespace {
void basis(Vec3 axis, Vec3& u, Vec3& v) {
    const Vec3 a = normalize(axis);
    const Vec3 t = std::fabs(a.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    u = normalize(cross(a, t));
    v = cross(a, u);
}
} // namespace

void Builder::cylinder(Vec3 a, Vec3 b, float radius, int segments, bool caps) {
    Vec3 u, v;
    basis(b - a, u, v);
    std::vector<std::vector<Vec3>> rings(2);
    for (int k = 0; k < segments; ++k) {
        const float t = 2 * kPi * (F(k) + 0.5f) / F(segments);
        const Vec3 o = (u * std::cos(t) + v * std::sin(t)) * radius;
        rings[0].push_back(a + o);
        rings[1].push_back(b + o);
    }
    loft(rings, caps, caps);
}

void Builder::cone(Vec3 base, Vec3 tip, float radius, int segments) {
    Vec3 u, v;
    basis(tip - base, u, v);
    std::vector<Vec3> ring0;
    for (int k = 0; k < segments; ++k) {
        const float t = 2 * kPi * (F(k) + 0.5f) / F(segments);
        ring0.push_back(base + (u * std::cos(t) + v * std::sin(t)) * radius);
    }
    const Vec3 inside = base + (tip - base) * 0.25f;
    for (int k = 0; k < segments; ++k) {
        const Vec3 p0 = ring0[static_cast<std::size_t>(k)], p1 = ring0[static_cast<std::size_t>((k + 1) % segments)];
        tri(p0, p1, tip, inside);
        tri(p0, p1, base, inside);
    }
}

void Builder::wing(Vec3 rootLe, float rootChord, Vec3 tipLe, float tipChord, float thickness) {
    // A six-point aerofoil, cambered slightly, thickness scaling with chord.
    const Vec3 span = normalize(tipLe - rootLe);
    Vec3 thick = cross(span, Vec3{1, 0, 0});
    if (length(thick) < 1e-3f) thick = {0, 0, 1};
    thick = normalize(thick);
    if (thick.z < 0 || (std::fabs(thick.z) < 1e-3f && thick.y < 0)) thick = -thick;
    auto section = [&](Vec3 le, float chord) {
        const float t = thickness * chord / rootChord;
        const std::array<Vec2, 6> foil{{{0.0f, 0.0f}, {0.22f, 0.55f}, {0.6f, 0.38f}, {1.0f, 0.0f}, {0.6f, -0.2f}, {0.22f, -0.35f}}};
        std::vector<Vec3> r;
        r.reserve(foil.size());
        for (const Vec2& p : foil) r.push_back(le + Vec3{-p.x * chord, 0, 0} + thick * (p.y * t));
        return r;
    };
    const std::vector<Vec3> rings[2] = {section(rootLe, rootChord), section(tipLe, tipChord)};
    loft(rings, true, true);
}

void Builder::icosphere(Vec3 c, float radius, int subdivisions, float jitter, std::uint32_t seed) {
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    std::vector<Vec3> p{{-1, t, 0},  {1, t, 0},  {-1, -t, 0}, {1, -t, 0}, {0, -1, t},  {0, 1, t},
                        {0, -1, -t}, {0, 1, -t}, {t, 0, -1},  {t, 0, 1},  {-t, 0, -1}, {-t, 0, 1}};
    std::vector<std::array<int, 3>> f{{0, 11, 5},  {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                                      {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
                                      {3, 8, 9},   {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
    for (Vec3& q : p) q = normalize(q);
    for (int s = 0; s < subdivisions; ++s) {
        std::map<std::pair<int, int>, int> mid;
        auto midpoint = [&](int a, int b) {
            const auto key = std::minmax(a, b);
            if (auto it = mid.find(key); it != mid.end()) return it->second;
            p.push_back(normalize((p[static_cast<std::size_t>(a)] + p[static_cast<std::size_t>(b)]) * 0.5f));
            const int id = static_cast<int>(p.size()) - 1;
            mid[key] = id;
            return id;
        };
        std::vector<std::array<int, 3>> g;
        for (auto [a, b, cc] : f) {
            const int ab = midpoint(a, b), bc = midpoint(b, cc), ca = midpoint(cc, a);
            g.push_back({a, ab, ca});
            g.push_back({b, bc, ab});
            g.push_back({cc, ca, bc});
            g.push_back({ab, bc, ca});
        }
        f = std::move(g);
    }
    std::uint32_t h = seed * 2654435761u + 1u;
    for (Vec3& q : p) {
        h ^= h << 13;
        h ^= h >> 17;
        h ^= h << 5;
        const float j = 1.0f + jitter * (F(static_cast<int>(h % 2001u)) / 1000.0f - 1.0f);
        q = c + q * (radius * j);
    }
    for (auto [a, b, cc] : f) tri(p[static_cast<std::size_t>(a)], p[static_cast<std::size_t>(b)], p[static_cast<std::size_t>(cc)], c);
}

void Builder::wheel(Vec3 c, float radius, float width, int segments) {
    const Rgb8 saved = colour_;
    const Material savedM = material_;
    color(palette::rubber);
    cylinder(c - Vec3{0, width * 0.5f, 0}, c + Vec3{0, width * 0.5f, 0}, radius, segments);
    color(palette::steel);
    cylinder(c - Vec3{0, width * 0.52f, 0}, c + Vec3{0, width * 0.52f, 0}, radius * 0.5f, 6);
    color(saved, savedM);
}

Mesh Builder::finish() {
    Mesh m;
    m.vertices = std::move(v_);
    m.indices.resize(m.vertices.size());
    for (std::size_t i = 0; i < m.indices.size(); ++i) m.indices[i] = static_cast<std::uint32_t>(i);
    for (const Vertex& v : m.vertices) {
        m.bounds.min = {std::min(m.bounds.min.x, v.px), std::min(m.bounds.min.y, v.py), std::min(m.bounds.min.z, v.pz)};
        m.bounds.max = {std::max(m.bounds.max.x, v.px), std::max(m.bounds.max.y, v.py), std::max(m.bounds.max.z, v.pz)};
    }
    v_.clear();
    stack_ = {Mat4::identity()};
    return m;
}

std::vector<Vec3> ring(float x, float w, float h, float z, int points, float squareness) {
    std::vector<Vec3> r;
    const float e = 1.0f - squareness;
    for (int k = 0; k < points; ++k) {
        const float a = 2 * kPi * (F(k) + 0.5f) / F(points);
        const float c = std::cos(a), s = std::sin(a);
        const float cy = std::copysign(std::pow(std::fabs(c), e), c), sz = std::copysign(std::pow(std::fabs(s), e), s);
        r.push_back({x, w * cy, z + h * sz});
    }
    return r;
}

// ---------------------------------------------------------------------------------------------
// The models

namespace {

using namespace palette;
using Rings = std::vector<std::vector<Vec3>>;

void nose(Rings& r, float x, float z = 0, int points = 8) {
    r.push_back(ring(x, 0.001f, 0.001f, z, points));
}

void verticalLathe(Builder& b, std::span<const Vec2> profile, int segments, float roll = 0) {
    // Lathe about +z: author about x, then turn x up.
    b.at({}, axisAngle({0, 1, 0}, -kPi / 2), [&] { b.lathe(profile, segments, roll); });
}

void kite(Builder& b) {
    // Pod.
    Rings pod;
    nose(pod, 0.46f, -0.01f);
    pod.push_back(ring(0.38f, 0.055f, 0.045f, -0.01f));
    pod.push_back(ring(0.2f, 0.085f, 0.07f, 0.0f));
    pod.push_back(ring(-0.12f, 0.08f, 0.065f, 0.005f));
    pod.push_back(ring(-0.32f, 0.035f, 0.03f, 0.01f));
    b.color(graphite);
    b.loft(pod);
    // Wing: inner panel in bone, outer quarter in the section colour, swept and with a little dihedral.
    b.mirrored([&] {
        b.color(bone);
        b.wing({0.2f, 0.05f, 0.0f}, 0.44f, {-0.08f, 0.58f, 0.028f}, 0.24f, 0.034f);
        b.team();
        b.wing({-0.08f, 0.58f, 0.028f}, 0.24f, {-0.2f, 0.8f, 0.038f}, 0.15f, 0.034f);
        // Winglet.
        b.color(graphite);
        b.wing({-0.2f, 0.8f, 0.038f}, 0.15f, {-0.29f, 0.81f, 0.17f}, 0.07f, 0.012f);
        b.color(navGreen, Material::Emissive);
        b.box({-0.2f, 0.805f, 0.04f}, {0.025f, 0.012f, 0.012f});
    });
    // Laser turret: a gimbal ball under the nose with the emitter lens in the section colour.
    b.color(gunmetal);
    b.icosphere({0.27f, 0.0f, -0.075f}, 0.045f, 1);
    b.color(bone, Material::TeamEmissive);
    b.cylinder({0.305f, 0.0f, -0.08f}, {0.322f, 0.0f, -0.082f}, 0.022f, 8);
    // Acoustic projector: a flat horn on the belly.
    b.color(steel);
    b.wedgeBox({-0.05f, 0.0f, -0.085f}, {0.16f, 0.07f, 0.03f}, 0.02f, 0.01f);
    // Pusher propeller disc.
    b.color(ash);
    b.at({-0.335f, 0, 0.01f}, axisAngle({0, 1, 0}, kPi / 2), [&] { b.cylinder({0, 0, -0.003f}, {0, 0, 0.003f}, 0.13f, 10); });
}

void quadcopter(Builder& b) {
    b.color(graphite);
    b.wedgeBox({0, 0, 0}, {0.2f, 0.13f, 0.06f}, 0.03f, 0.02f);
    b.color(bone);
    b.wedgeBox({0.01f, 0, 0.045f}, {0.13f, 0.09f, 0.03f}, 0.03f, 0.02f);
    for (int i = 0; i < 4; ++i) {
        const float yaw = kPi / 4 + F(i) * kPi / 2;
        const bool front = i == 0 || i == 3;
        b.at({0, 0, 0}, axisAngle({0, 0, 1}, yaw), [&] {
            b.color(front ? bone : graphite, front ? Material::Team : Material::Matte);
            b.box({0.12f, 0, 0}, {0.2f, 0.024f, 0.018f});
            b.color(gunmetal);
            b.cylinder({0.22f, 0, -0.02f}, {0.22f, 0, 0.026f}, 0.024f, 8);
            b.color(hex(0x5A616B));
            b.cylinder({0.22f, 0, 0.03f}, {0.22f, 0, 0.033f}, 0.095f, 12);
            b.color(front ? bone : navRed, front ? Material::TeamEmissive : Material::Emissive);
            b.box({0.22f, 0, -0.024f}, {0.012f, 0.012f, 0.008f});
        });
    }
    b.color(gunmetal);
    b.mirrored([&] {
        b.box({0, 0.07f, -0.075f}, {0.2f, 0.012f, 0.012f});
        b.box({0.05f, 0.055f, -0.05f}, {0.012f, 0.012f, 0.05f});
        b.box({-0.05f, 0.055f, -0.05f}, {0.012f, 0.012f, 0.05f});
    });
    b.color(glass);
    b.icosphere({0.1f, 0, -0.04f}, 0.022f, 1);
}

void fixedWing(Builder& b) {
    Rings f;
    nose(f, 0.95f, -0.02f);
    f.push_back(ring(0.85f, 0.07f, 0.07f, -0.01f));
    f.push_back(ring(0.55f, 0.11f, 0.11f, 0.0f));
    f.push_back(ring(0.1f, 0.11f, 0.12f, 0.01f));
    f.push_back(ring(-0.35f, 0.07f, 0.07f, 0.03f));
    f.push_back(ring(-0.9f, 0.03f, 0.03f, 0.06f));
    b.color(bone);
    b.loft(f);
    b.mirrored([&] {
        b.color(bone);
        b.wing({0.28f, 0.08f, 0.1f}, 0.34f, {0.18f, 1.2f, 0.15f}, 0.25f, 0.04f);
        b.team();
        b.wing({0.18f, 1.2f, 0.15f}, 0.25f, {0.14f, 1.5f, 0.17f}, 0.2f, 0.04f);
        b.color(bone);
        b.wing({-0.68f, 0.02f, 0.07f}, 0.24f, {-0.84f, 0.36f, 0.32f}, 0.13f, 0.02f);
        b.color(navRed, Material::Emissive);
        b.box({0.06f, 1.5f, 0.17f}, {0.03f, 0.015f, 0.015f});
    });
    b.color(gunmetal);
    b.icosphere({0.62f, 0, -0.12f}, 0.065f, 1);
    b.color(glass, Material::Emissive);
    b.cylinder({0.67f, 0, -0.14f}, {0.69f, 0, -0.145f}, 0.025f, 8);
    b.color(ash);
    b.at({-0.93f, 0, 0.06f}, axisAngle({0, 1, 0}, kPi / 2), [&] { b.cylinder({0, 0, -0.004f}, {0, 0, 0.004f}, 0.2f, 10); });
}

void fighter(Builder& b, Rgb8 body) {
    Rings f;
    nose(f, 8.4f, 0.15f);
    f.push_back(ring(7.2f, 0.42f, 0.42f, 0.15f, 8, 0.2f));
    f.push_back(ring(5.0f, 0.82f, 0.72f, 0.22f, 8, 0.45f));
    f.push_back(ring(3.0f, 1.2f, 0.85f, 0.2f, 8, 0.6f));
    f.push_back(ring(1.0f, 1.65f, 0.78f, 0.08f, 8, 0.7f));
    f.push_back(ring(-2.0f, 1.75f, 0.7f, 0.05f, 8, 0.7f));
    f.push_back(ring(-5.0f, 1.5f, 0.6f, 0.05f, 8, 0.65f));
    f.push_back(ring(-6.9f, 1.25f, 0.5f, 0.02f, 8, 0.6f));
    b.color(body);
    b.loft(f);
    b.color(graphite);
    b.lathe(std::array<Vec2, 3>{{{8.42f, 0.0f}, {7.9f, 0.18f}, {7.7f, 0.0f}}}, 8);
    // Canopy.
    Rings c;
    nose(c, 5.6f, 0.8f);
    c.push_back(ring(4.8f, 0.42f, 0.32f, 0.92f, 8, 0.1f));
    c.push_back(ring(3.4f, 0.5f, 0.42f, 0.95f, 8, 0.1f));
    c.push_back(ring(2.0f, 0.3f, 0.25f, 0.88f, 8, 0.1f));
    b.color(glass);
    b.loft(c);
    b.mirrored([&] {
        b.color(body);
        b.wing({1.6f, 1.5f, 0.0f}, 6.2f, {-2.7f, 5.4f, -0.12f}, 1.5f, 0.3f);
        b.wing({-4.5f, 1.3f, 0.0f}, 2.7f, {-6.4f, 3.5f, -0.05f}, 1.15f, 0.14f);
        b.wing({-3.2f, 1.0f, 0.55f}, 3.3f, {-5.0f, 1.55f, 2.7f}, 1.75f, 0.13f);
        b.team();
        b.wing({-5.0f, 1.55f, 2.7f}, 1.75f, {-5.55f, 1.7f, 3.5f}, 1.25f, 0.13f);
        // Intake.
        b.color(body);
        b.wedgeBox({2.2f, 1.3f, -0.35f}, {2.8f, 0.8f, 0.85f}, 0.0f, 0.12f);
        b.color(rubber);
        b.box({3.62f, 1.3f, -0.35f}, {0.06f, 0.6f, 0.7f});
        // Nozzle with afterburner glow.
        b.color(gunmetal);
        b.cylinder({-6.7f, 0.55f, 0.05f}, {-7.5f, 0.55f, 0.05f}, 0.5f, 10);
        b.color(engine, Material::Emissive);
        b.cylinder({-7.5f, 0.55f, 0.05f}, {-7.54f, 0.55f, 0.05f}, 0.38f, 10);
        // Wingtip missile.
        b.color(bone);
        b.cylinder({-0.4f, 5.45f, -0.12f}, {-3.0f, 5.45f, -0.12f}, 0.09f, 6);
        b.cone({-0.4f, 5.45f, -0.12f}, {0.1f, 5.45f, -0.12f}, 0.09f, 6);
        b.color(navGreen, Material::Emissive);
        b.box({-2.0f, 5.33f, -0.1f}, {0.2f, 0.06f, 0.06f});
    });
}

void striker(Builder& b) {
    const Rgb8 body = hex(0x6E7466);
    Rings f;
    nose(f, 7.6f, 0.0f);
    f.push_back(ring(6.6f, 0.38f, 0.38f, 0.05f, 8, 0.2f));
    f.push_back(ring(4.0f, 0.75f, 0.8f, 0.15f, 8, 0.4f));
    f.push_back(ring(0.0f, 0.9f, 0.9f, 0.1f, 8, 0.45f));
    f.push_back(ring(-4.0f, 0.65f, 0.75f, 0.2f, 8, 0.45f));
    f.push_back(ring(-6.6f, 0.25f, 0.4f, 0.4f, 8, 0.4f));
    b.color(body);
    b.loft(f);
    Rings c;
    nose(c, 4.9f, 0.85f);
    c.push_back(ring(4.2f, 0.4f, 0.35f, 0.95f, 8, 0.1f));
    c.push_back(ring(2.8f, 0.42f, 0.4f, 0.98f, 8, 0.1f));
    c.push_back(ring(1.9f, 0.25f, 0.2f, 0.9f, 8, 0.1f));
    b.color(glass);
    b.loft(c);
    b.color(body);
    b.wing({-4.0f, 0.0f, 0.6f}, 2.6f, {-6.2f, 0.0f, 3.6f}, 1.3f, 0.14f);
    b.team();
    b.wing({-6.2f, 0.0f, 3.6f}, 1.3f, {-6.5f, 0.0f, 4.1f}, 1.0f, 0.14f);
    b.mirrored([&] {
        b.color(body);
        b.wing({1.4f, 0.8f, -0.1f}, 3.6f, {-0.4f, 6.8f, 0.05f}, 1.5f, 0.32f);
        b.wing({-4.4f, 0.4f, 0.25f}, 1.9f, {-5.6f, 2.6f, 0.3f}, 1.0f, 0.12f);
        // Engine pod on the rear fuselage.
        b.color(gunmetal);
        b.lathe(std::array<Vec2, 4>{{{-1.4f, 0.0f}, {-1.5f, 0.55f}, {-3.6f, 0.55f}, {-3.9f, 0.4f}}}, 10);
        b.color(engine, Material::Emissive);
        b.cylinder({-3.9f, 0, 0}, {-3.93f, 0, 0}, 0.32f, 10);
        // Stores.
        for (float y : {2.6f, 4.2f}) {
            b.color(olive);
            b.cylinder({1.0f, y, -0.55f}, {-1.4f, y, -0.55f}, 0.17f, 8);
            b.cone({1.0f, y, -0.55f}, {1.5f, y, -0.55f}, 0.17f, 8);
            b.color(gunmetal);
            b.box({-0.2f, y, -0.32f}, {1.2f, 0.06f, 0.3f});
        }
        b.color(navRed, Material::Emissive);
        b.box({-0.6f, 6.8f, 0.05f}, {0.2f, 0.06f, 0.06f});
    });
}

void helicopter(Builder& b) {
    const Rgb8 body = hex(0x55604A);
    Rings f;
    f.push_back(ring(3.4f, 0.3f, 0.4f, 0.2f, 8, 0.3f));
    f.push_back(ring(2.8f, 0.85f, 0.9f, 0.4f, 8, 0.5f));
    f.push_back(ring(0.5f, 1.05f, 1.05f, 0.5f, 8, 0.6f));
    f.push_back(ring(-1.6f, 0.9f, 0.9f, 0.6f, 8, 0.6f));
    f.push_back(ring(-2.6f, 0.3f, 0.35f, 0.9f, 8, 0.4f));
    f.push_back(ring(-7.0f, 0.12f, 0.15f, 1.1f, 6, 0.3f));
    b.color(body);
    b.loft(f);
    b.color(glass);
    b.wedgeBox({2.5f, 0, 0.9f}, {1.0f, 1.5f, 0.7f}, 0.35f, 0.15f, -0.2f);
    b.color(body);
    b.wedgeBox({-0.3f, 0, 1.65f}, {2.6f, 0.9f, 0.5f}, 0.4f, 0.15f);
    b.wing({-6.4f, 0.0f, 1.1f}, 1.0f, {-7.1f, 0.0f, 2.6f}, 0.6f, 0.08f);
    b.team();
    b.box({-0.4f, 0, 0.65f}, {1.8f, 2.12f, 0.25f});
    b.color(gunmetal);
    b.cylinder({-0.3f, 0, 1.9f}, {-0.3f, 0, 2.25f}, 0.18f, 8);
    for (int i = 0; i < 4; ++i)
        b.at({-0.3f, 0, 2.2f}, axisAngle({0, 0, 1}, F(i) * kPi / 2 + 0.3f), [&] { b.box({3.4f, 0, 0}, {6.6f, 0.32f, 0.04f}); });
    b.at({-7.0f, 0.18f, 1.8f}, Quat{}, [&] {
        b.color(ash);
        b.cylinder({0, -0.02f, 0}, {0, 0.02f, 0}, 0.75f, 10);
    });
    b.color(gunmetal);
    b.mirrored([&] {
        b.box({0.3f, 0.95f, -0.55f}, {3.6f, 0.08f, 0.08f});
        b.box({1.3f, 0.85f, -0.3f}, {0.08f, 0.08f, 0.5f});
        b.box({-0.8f, 0.85f, -0.3f}, {0.08f, 0.08f, 0.5f});
    });
    b.color(navRed, Material::Emissive);
    b.box({-7.0f, 0, 2.6f}, {0.12f, 0.12f, 0.12f});
}

void interceptor(Builder& b) {
    Rings f;
    nose(f, 1.25f, 0.0f, 4);
    f.push_back(ring(0.8f, 0.22f, 0.08f, 0.0f, 4, 0.0f));
    f.push_back(ring(-0.2f, 0.75f, 0.12f, 0.0f, 4, 0.0f));
    f.push_back(ring(-0.95f, 1.15f, 0.06f, 0.0f, 4, 0.0f));
    b.color(graphite);
    b.loft(f);
    b.team();
    Rings s;
    nose(s, 0.9f, 0.08f, 6);
    s.push_back(ring(0.4f, 0.12f, 0.06f, 0.1f, 6, 0.2f));
    s.push_back(ring(-0.6f, 0.16f, 0.08f, 0.1f, 6, 0.2f));
    b.loft(s);
    b.mirrored([&] {
        b.color(graphite);
        b.wing({-0.5f, 0.45f, 0.05f}, 0.45f, {-0.85f, 0.62f, 0.38f}, 0.2f, 0.02f);
    });
    b.color(engine, Material::Emissive);
    b.cylinder({-0.7f, 0, 0.0f}, {-0.98f, 0, 0.0f}, 0.1f, 8);
    b.color(bone, Material::TeamEmissive);
    b.box({0.75f, 0, 0.06f}, {0.1f, 0.05f, 0.02f});
}

void missile(Builder& b) {
    b.color(bone);
    b.lathe(std::array<Vec2, 5>{{{1.2f, 0.0f}, {0.95f, 0.075f}, {0.7f, 0.085f}, {-1.45f, 0.085f}, {-1.5f, 0.07f}}}, 8, kPi / 8);
    b.color(graphite);
    b.lathe(std::array<Vec2, 3>{{{1.5f, 0.0f}, {1.2f, 0.05f}, {1.15f, 0.0f}}}, 8, kPi / 8);
    b.team();
    b.lathe(std::array<Vec2, 2>{{{0.6f, 0.088f}, {0.45f, 0.088f}}}, 8, kPi / 8);
    for (int i = 0; i < 4; ++i)
        b.at({}, axisAngle({1, 0, 0}, F(i) * kPi / 2), [&] {
            b.color(gunmetal);
            b.panel({-1.05f, 0, 0.08f}, {-1.45f, 0, 0.08f}, {-1.45f, 0, 0.3f}, {-1.25f, 0, 0.3f});
            b.panel({0.75f, 0, 0.08f}, {0.55f, 0, 0.08f}, {0.55f, 0, 0.17f}, {0.65f, 0, 0.17f});
        });
    b.color(engine, Material::Emissive);
    b.cylinder({-1.5f, 0, 0}, {-1.56f, 0, 0}, 0.06f, 8);
}

void bomb(Builder& b) {
    b.color(olive);
    b.lathe(std::array<Vec2, 6>{{{1.25f, 0.0f}, {1.12f, 0.1f}, {0.7f, 0.17f}, {-0.75f, 0.17f}, {-1.15f, 0.1f}, {-1.2f, 0.09f}}}, 10);
    b.color(glass, Material::Emissive);
    b.lathe(std::array<Vec2, 2>{{{1.26f, 0.0f}, {1.2f, 0.06f}}}, 8);
    b.color(gunmetal);
    for (int i = 0; i < 4; ++i)
        b.at({}, axisAngle({1, 0, 0}, kPi / 4 + F(i) * kPi / 2), [&] {
            b.panel({-0.8f, 0, 0.1f}, {-1.2f, 0, 0.09f}, {-1.25f, 0, 0.32f}, {-1.0f, 0, 0.32f});
            b.panel({1.0f, 0, 0.12f}, {0.85f, 0, 0.15f}, {0.85f, 0, 0.24f}, {0.95f, 0, 0.24f});
        });
}

void solarWing(Builder& b, float y0, float y1, float w) {
    b.color(graphite);
    b.box({0, (y0 + y1) * 0.5f, 0}, {w + 0.06f, y1 - y0 + 0.06f, 0.03f});
    const int cells = std::max(2, static_cast<int>((y1 - y0) / 0.9f));
    for (int i = 0; i < cells; ++i) {
        const float a = y0 + (y1 - y0) * F(i) / F(cells) + 0.03f, c = y0 + (y1 - y0) * F(i + 1) / F(cells) - 0.03f;
        b.color(i % 2 ? cell : hex(0x24457F));
        b.box({0, (a + c) * 0.5f, 0.0f}, {w - 0.06f, c - a, 0.05f});
    }
}

void relaySat(Builder& b) {
    b.color(gold);
    b.wedgeBox({0, 0, 0}, {1.1f, 0.9f, 0.5f}, 0.04f, 0.04f);
    b.color(bone);
    b.box({0, 0, -0.29f}, {1.0f, 0.8f, 0.06f});
    b.team();
    b.box({0, 0, 0.26f}, {1.12f, 0.92f, 0.03f});
    b.mirrored([&] {
        b.color(steel);
        b.cylinder({0, 0.45f, 0}, {0, 1.0f, 0}, 0.03f, 6);
        solarWing(b, 1.0f, 4.6f, 1.0f);
    });
    b.color(bone);
    b.at({0.75f, 0, 0}, axisAngle({0, 1, 0}, -kPi / 2),
         [&] { b.lathe(std::array<Vec2, 3>{{{0.0f, 0.0f}, {0.12f, 0.35f}, {0.16f, 0.4f}}}, 12); });
    b.color(bone, Material::TeamEmissive);
    b.box({0.58f, 0, 0.2f}, {0.05f, 0.05f, 0.05f});
}

void imagingSat(Builder& b) {
    b.color(white);
    b.lathe(std::array<Vec2, 4>{{{1.6f, 0.55f}, {1.55f, 0.62f}, {-0.6f, 0.62f}, {-0.7f, 0.5f}}}, 12);
    b.color(rubber);
    b.cylinder({1.6f, 0, 0}, {1.615f, 0, 0}, 0.5f, 12);
    b.color(glass, Material::Emissive);
    b.cylinder({1.615f, 0, 0}, {1.625f, 0, 0}, 0.16f, 10);
    b.color(gold);
    b.box({-1.2f, 0, 0}, {1.0f, 1.1f, 1.1f});
    b.team();
    b.box({0.0f, 0, 0}, {0.2f, 1.27f, 1.27f});
    b.mirrored([&] {
        b.color(steel);
        b.cylinder({-1.2f, 0.55f, 0}, {-1.2f, 1.0f, 0}, 0.03f, 6);
        b.at({-1.2f, 0, 0}, Quat{}, [&] { solarWing(b, 1.0f, 3.4f, 1.0f); });
    });
}

void truckBase(Builder& b, Rgb8 cab, float bedLength) {
    b.color(graphite);
    b.box({-0.6f, 0, 0.75f}, {bedLength + 3.0f, 1.0f, 0.3f});
    b.color(cab);
    b.wedgeBox({2.65f, 0, 1.75f}, {1.7f, 2.4f, 1.7f}, 0.25f, 0.1f, -0.15f);
    b.wedgeBox({3.55f, 0, 1.25f}, {0.6f, 2.3f, 0.7f}, 0.15f, 0.05f, -0.1f);
    b.color(glass);
    b.box({3.33f, 0, 2.15f}, {0.3f, 2.0f, 0.55f});
    b.color(rubber);
    b.box({3.86f, 0, 1.1f}, {0.06f, 1.6f, 0.35f});
    b.mirrored([&] {
        b.color(glass);
        b.box({2.7f, 1.21f, 2.15f}, {1.0f, 0.04f, 0.5f});
        b.color(lamp, Material::Emissive);
        b.box({3.86f, 0.95f, 1.3f}, {0.04f, 0.22f, 0.14f});
        b.color(navRed, Material::Emissive);
        b.box({-bedLength + 0.1f, 1.1f, 0.85f}, {0.04f, 0.15f, 0.1f});
        for (float x : {2.6f, -bedLength + 1.9f, -bedLength + 0.75f}) b.wheel({x, 1.08f, 0.55f}, 0.55f, 0.42f);
    });
}

void truck(Builder& b) {
    truckBase(b, white, 4.0f);
    b.color(white);
    b.wedgeBox({-0.2f, 0, 2.15f}, {4.0f, 2.5f, 2.5f}, 0.0f, 0.04f);
    b.team();
    b.box({-0.2f, 0, 1.6f}, {4.02f, 2.52f, 0.3f});
    b.color(bone);
    b.box({-0.2f, 0, 3.42f}, {3.9f, 2.3f, 0.06f});
}

void samLauncher(Builder& b) {
    const Rgb8 body = hex(0x6B6A4E);
    truckBase(b, body, 4.4f);
    b.color(body);
    b.box({-0.4f, 0, 1.15f}, {4.6f, 2.4f, 0.5f});
    b.at({-2.2f, 0, 1.5f}, axisAngle({0, 1, 0}, -0.85f), [&] {
        b.color(body);
        b.box({2.2f, 0, 0.55f}, {4.6f, 1.9f, 1.1f});
        b.color(rubber);
        for (float y : {-0.48f, 0.48f})
            for (float z : {0.3f, 0.82f}) b.cylinder({4.5f, y, z}, {4.52f, y, z}, 0.36f, 8);
        b.team();
        b.box({2.2f, 0, 1.12f}, {4.62f, 1.92f, 0.06f});
    });
    b.color(gunmetal);
    b.cylinder({-1.6f, 0, 1.4f}, {-0.6f, 0, 2.6f}, 0.1f, 6);
}

void tank(Builder& b) {
    const Rgb8 body = khaki;
    b.color(body);
    b.wedgeBox({0, 0, 1.05f}, {7.2f, 3.0f, 0.9f}, 0.6f, 0.0f, 0.2f);
    b.mirrored([&] {
        b.color(rubber);
        b.wedgeBox({0, 1.5f, 0.55f}, {7.0f, 0.65f, 0.85f}, -0.25f, 0.0f);
        b.color(body);
        b.box({0, 1.52f, 1.05f}, {7.4f, 0.7f, 0.12f});
        b.color(steel);
        for (int i = 0; i < 6; ++i) b.cylinder({-2.6f + F(i) * 1.05f, 1.86f, 0.42f}, {-2.6f + F(i) * 1.05f, 1.9f, 0.42f}, 0.34f, 8);
    });
    b.color(body);
    b.wedgeBox({-0.4f, 0, 1.95f}, {3.8f, 2.8f, 0.85f}, 0.55f, 0.35f, -0.15f);
    b.team();
    b.mirrored([&] { b.box({-0.4f, 1.12f, 2.05f}, {2.2f, 0.06f, 0.35f}); });
    b.color(body);
    b.box({1.6f, 0, 1.95f}, {0.6f, 0.9f, 0.6f});
    b.cylinder({1.8f, 0, 1.95f}, {6.4f, 0, 1.95f}, 0.11f, 8);
    b.color(gunmetal);
    b.cylinder({4.0f, 0, 1.95f}, {4.6f, 0, 1.95f}, 0.17f, 8);
    b.cylinder({6.2f, 0, 1.95f}, {6.5f, 0, 1.95f}, 0.15f, 8);
    b.cylinder({-0.9f, 0.6f, 2.35f}, {-0.9f, 0.6f, 2.65f}, 0.38f, 8);
    b.color(glass, Material::Emissive);
    b.box({-0.6f, -0.7f, 2.45f}, {0.3f, 0.3f, 0.2f});
}

void apc(Builder& b) {
    const Rgb8 body = hex(0x7A7656);
    b.color(body);
    b.wedgeBox({0, 0, 1.55f}, {7.2f, 2.8f, 1.5f}, 0.5f, 0.25f, -0.35f);
    b.wedgeBox({3.4f, 0, 1.2f}, {1.2f, 2.8f, 0.8f}, 0.5f, 0.0f, -0.25f);
    b.team();
    b.mirrored([&] { b.box({-0.6f, 1.33f, 1.85f}, {3.6f, 0.06f, 0.3f}); });
    b.color(body);
    b.wedgeBox({0.6f, 0, 2.55f}, {1.6f, 1.4f, 0.5f}, 0.3f, 0.2f);
    b.color(gunmetal);
    b.cylinder({1.3f, 0, 2.6f}, {3.4f, 0, 2.6f}, 0.06f, 6);
    b.mirrored([&] {
        for (float x : {2.5f, 1.2f, -1.2f, -2.5f}) b.wheel({x, 1.25f, 0.55f}, 0.55f, 0.4f);
        b.color(lamp, Material::Emissive);
        b.box({4.0f, 1.1f, 1.45f}, {0.04f, 0.2f, 0.12f});
    });
}

void aaaGun(Builder& b) {
    const Rgb8 body = hex(0x6B6A4E);
    b.color(basalt);
    verticalLathe(b, std::array<Vec2, 3>{{{0.0f, 2.4f}, {0.5f, 2.2f}, {0.5f, 0.0f}}}, 8, kPi / 8);
    b.color(body);
    b.wedgeBox({0, 0, 1.0f}, {2.2f, 2.0f, 1.0f}, 0.4f, 0.3f);
    b.team();
    b.box({0, 0, 1.52f}, {1.4f, 1.4f, 0.05f});
    b.at({0.6f, 0, 1.2f}, axisAngle({0, 1, 0}, -0.7f), [&] {
        b.color(gunmetal);
        b.mirrored([&] {
            b.cylinder({0, 0.45f, 0}, {3.6f, 0.45f, 0}, 0.08f, 6);
            b.cylinder({2.8f, 0.45f, 0}, {3.4f, 0.45f, 0}, 0.11f, 6);
        });
        b.color(body);
        b.box({0.2f, 0, 0}, {0.9f, 1.1f, 0.5f});
    });
    b.color(bone);
    b.at({-1.0f, 0, 1.9f}, axisAngle({0, 1, 0}, -0.4f),
         [&] { b.lathe(std::array<Vec2, 3>{{{0.0f, 0.0f}, {0.1f, 0.4f}, {0.14f, 0.45f}}}, 10); });
}

void radar(Builder& b) {
    b.color(olive);
    b.box({0, 0, 0.9f}, {4.2f, 2.2f, 0.6f});
    b.color(gunmetal);
    b.box({2.6f, 0, 0.75f}, {1.4f, 0.12f, 0.12f});
    b.mirrored([&] { b.wheel({-0.6f, 1.15f, 0.5f}, 0.5f, 0.35f); });
    b.color(steel);
    b.cylinder({0, 0, 1.2f}, {0, 0, 2.6f}, 0.18f, 8);
    b.at({0, 0, 3.4f}, axisAngle({0, 1, 0}, -0.3f), [&] {
        b.color(bone);
        b.box({0.0f, 0, 0}, {0.25f, 3.6f, 1.8f});
        b.team();
        b.box({0.0f, 0, 0.92f}, {0.27f, 3.62f, 0.06f});
        b.color(graphite);
        b.box({0.14f, 0, 0}, {0.02f, 3.3f, 1.5f});
    });
}

void acousticArray(Builder& b) {
    b.color(gunmetal);
    for (int i = 0; i < 3; ++i) {
        const float a = F(i) * 2 * kPi / 3;
        b.cylinder({0, 0, 1.4f}, {1.2f * std::cos(a), 1.2f * std::sin(a), 0}, 0.04f, 5);
    }
    b.color(steel);
    b.cylinder({0, 0, 0.5f}, {0, 0, 4.2f}, 0.07f, 6);
    for (int i = 0; i < 8; ++i) {
        const float a = F(i) * kPi / 4;
        const Vec3 tip{1.1f * std::cos(a), 1.1f * std::sin(a), 3.8f};
        b.color(steel);
        b.cylinder({0, 0, 3.9f}, tip, 0.02f, 4);
        b.color(graphite);
        b.icosphere(tip, 0.1f, 1);
    }
    b.team();
    b.cylinder({0, 0, 4.2f}, {0, 0, 4.35f}, 0.16f, 8);
    b.color(navRed, Material::Emissive);
    b.icosphere({0, 0, 4.45f}, 0.06f, 0);
}

void jammer(Builder& b) {
    const Rgb8 body = hex(0x6B6A4E);
    truckBase(b, body, 3.6f);
    b.color(body);
    b.wedgeBox({-0.2f, 0, 2.0f}, {3.6f, 2.4f, 2.2f}, 0.1f, 0.1f);
    b.team();
    b.box({-0.2f, 0, 2.6f}, {3.62f, 2.42f, 0.2f});
    b.color(steel);
    b.cylinder({-1.4f, 0, 3.1f}, {-1.4f, 0, 8.0f}, 0.1f, 6);
    for (int i = 0; i < 3; ++i)
        b.at({-1.4f, 0, 7.4f}, axisAngle({0, 0, 1}, F(i) * 2 * kPi / 3), [&] {
            b.color(bone);
            b.box({0.35f, 0, 0}, {0.08f, 0.5f, 1.2f});
        });
    b.color(navRed, Material::Emissive);
    b.icosphere({-1.4f, 0, 8.1f}, 0.08f, 0);
}

void rover(Builder& b) {
    b.color(bone);
    b.wedgeBox({0, 0, 0.75f}, {1.6f, 1.0f, 0.4f}, 0.1f, 0.06f);
    b.color(gold);
    b.box({-0.1f, 0, 0.98f}, {1.2f, 0.85f, 0.06f});
    b.team();
    b.box({0.0f, 0, 0.6f}, {1.62f, 1.02f, 0.08f});
    b.mirrored([&] {
        b.color(gunmetal);
        b.cylinder({0.0f, 0.6f, 0.75f}, {0.75f, 0.75f, 0.32f}, 0.035f, 5);
        b.cylinder({0.0f, 0.6f, 0.75f}, {-0.4f, 0.75f, 0.45f}, 0.035f, 5);
        b.cylinder({-0.4f, 0.75f, 0.45f}, {-0.1f, 0.75f, 0.32f}, 0.035f, 5);
        b.cylinder({-0.4f, 0.75f, 0.45f}, {-0.8f, 0.75f, 0.32f}, 0.035f, 5);
        for (float x : {0.75f, -0.1f, -0.8f}) b.wheel({x, 0.8f, 0.3f}, 0.3f, 0.22f, 10);
    });
    b.color(steel);
    b.cylinder({0.55f, 0.3f, 0.95f}, {0.55f, 0.3f, 1.55f}, 0.035f, 6);
    b.color(bone);
    b.wedgeBox({0.6f, 0.3f, 1.62f}, {0.22f, 0.36f, 0.16f}, 0.03f, 0.03f);
    b.color(bone, Material::TeamEmissive);
    b.cylinder({0.71f, 0.22f, 1.63f}, {0.73f, 0.22f, 1.63f}, 0.04f, 8);
    b.cylinder({0.71f, 0.38f, 1.63f}, {0.73f, 0.38f, 1.63f}, 0.04f, 8);
    b.color(steel);
    b.cylinder({-0.5f, -0.3f, 0.95f}, {-0.5f, -0.3f, 1.6f}, 0.015f, 4);
}

void robotArm(Builder& b) {
    b.color(graphite);
    b.cylinder({0, 0, 0}, {0, 0, 0.25f}, 0.38f, 10);
    b.team();
    b.cylinder({0, 0, 0.25f}, {0, 0, 0.6f}, 0.26f, 10);
    b.color(gunmetal);
    b.cylinder({0, -0.22f, 0.75f}, {0, 0.22f, 0.75f}, 0.16f, 10);
    b.team();
    b.wedgeBox({0.25f, 0, 1.3f}, {0.24f, 0.26f, 1.1f}, 0.02f, 0.02f, 0.35f);
    b.at({0.55f, 0, 1.85f}, axisAngle({0, 1, 0}, 0.9f), [&] {
        b.color(gunmetal);
        b.cylinder({0, -0.17f, 0}, {0, 0.17f, 0}, 0.13f, 10);
        b.team();
        b.box({0.45f, 0, 0}, {0.9f, 0.18f, 0.18f});
        b.color(gunmetal);
        b.cylinder({0.92f, 0, 0}, {1.05f, 0, 0}, 0.09f, 8);
        b.color(graphite);
        b.box({1.12f, 0, 0}, {0.12f, 0.22f, 0.1f});
        b.mirrored([&] { b.box({1.27f, 0.08f, 0}, {0.18f, 0.03f, 0.06f}); });
        b.color(bone, Material::TeamEmissive);
        b.box({1.0f, 0, 0.1f}, {0.06f, 0.06f, 0.02f});
    });
}

void bunker(Builder& b) {
    b.color(basalt);
    verticalLathe(b, std::array<Vec2, 5>{{{0.0f, 5.0f}, {1.2f, 4.7f}, {2.3f, 3.6f}, {2.8f, 1.8f}, {2.9f, 0.0f}}}, 6, kPi / 6);
    b.team();
    verticalLathe(b, std::array<Vec2, 2>{{{1.3f, 4.66f}, {1.1f, 4.74f}}}, 6, kPi / 6);
    b.color(lamp, Material::Emissive);
    for (int i = 0; i < 3; ++i) b.at({}, axisAngle({0, 0, 1}, F(i) * 2 * kPi / 3), [&] { b.box({4.0f, 0, 1.6f}, {0.15f, 1.6f, 0.18f}); });
    b.color(steel);
    b.cylinder({0.6f, 0.6f, 2.8f}, {0.6f, 0.6f, 6.0f}, 0.05f, 4);
    b.color(navRed, Material::Emissive);
    b.icosphere({0.6f, 0.6f, 6.05f}, 0.08f, 0);
}

void turret(Builder& b) {
    b.color(basalt);
    verticalLathe(b, std::array<Vec2, 3>{{{0.0f, 1.9f}, {0.7f, 1.7f}, {0.8f, 0.0f}}}, 6, kPi / 6);
    b.color(gunmetal);
    b.wedgeBox({0, 0, 1.3f}, {2.0f, 1.6f, 0.9f}, 0.35f, 0.25f, -0.1f);
    b.team();
    b.mirrored([&] { b.box({-0.1f, 0.81f, 1.3f}, {1.4f, 0.04f, 0.3f}); });
    b.color(graphite);
    b.mirrored([&] { b.cylinder({0.8f, 0.3f, 1.4f}, {3.0f, 0.3f, 1.55f}, 0.09f, 6); });
    b.color(bone, Material::TeamEmissive);
    b.box({0.85f, 0, 1.6f}, {0.1f, 0.3f, 0.1f});
}

void legs(Builder& b, float y, float z, float reach, float thick, int pairs, float x0, float dx) {
    b.mirrored([&] {
        for (int i = 0; i < pairs; ++i) {
            const float x = x0 + F(i) * dx;
            const Vec3 hip{x, y, z}, knee{x + 0.1f * reach, y + reach * 0.7f, z + reach * 0.35f},
                foot{x + 0.25f * reach, y + reach * 1.1f, 0.0f};
            b.cylinder(hip, knee, thick, 5);
            b.cylinder(knee, foot, thick * 0.7f, 5);
        }
    });
}

void swarmer(Builder& b) {
    b.color(chitin);
    b.at({0, 0, 0.45f}, Quat{}, [&] {
        b.push(scaling({1.6f, 0.9f, 0.7f}));
        b.icosphere({}, 0.35f, 1, 0.12f, 7);
        b.pop();
    });
    b.color(hive);
    b.icosphere({0.55f, 0, 0.5f}, 0.2f, 1, 0.1f, 3);
    b.color(acid, Material::Emissive);
    b.mirrored([&] { b.icosphere({0.7f, 0.08f, 0.56f}, 0.045f, 0); });
    b.color(hive);
    for (int i = 0; i < 3; ++i) b.cone({-0.1f - F(i) * 0.18f, 0, 0.62f}, {-0.2f - F(i) * 0.2f, 0, 0.95f}, 0.07f, 4);
    b.color(chitin);
    legs(b, 0.2f, 0.4f, 0.45f, 0.035f, 3, 0.25f, -0.25f);
}

void bruiser(Builder& b) {
    b.color(hive);
    b.at({0, 0, 1.4f}, Quat{}, [&] {
        b.push(scaling({1.5f, 1.05f, 0.75f}));
        b.icosphere({}, 1.3f, 1, 0.1f, 11);
        b.pop();
    });
    b.color(chitin);
    b.at({0.2f, 0, 2.0f}, Quat{}, [&] {
        b.push(scaling({1.4f, 1.0f, 0.5f}));
        b.icosphere({}, 1.2f, 1, 0.15f, 5);
        b.pop();
    });
    b.icosphere({1.8f, 0, 1.4f}, 0.6f, 1, 0.1f, 9);
    b.color(bone);
    b.mirrored([&] { b.cone({2.1f, 0.35f, 1.6f}, {3.0f, 0.7f, 2.1f}, 0.15f, 5); });
    b.color(acid, Material::Emissive);
    b.mirrored([&] { b.icosphere({2.3f, 0.25f, 1.45f}, 0.08f, 0); });
    b.color(chitin);
    legs(b, 0.8f, 1.1f, 1.2f, 0.16f, 3, 0.9f, -0.9f);
}

void spitter(Builder& b) {
    b.color(chitin);
    b.at({-0.2f, 0, 0.8f}, Quat{}, [&] {
        b.push(scaling({1.2f, 0.9f, 0.8f}));
        b.icosphere({}, 0.6f, 1, 0.1f, 13);
        b.pop();
    });
    b.color(acid, Material::Emissive);
    b.at({-0.45f, 0, 1.35f}, Quat{}, [&] {
        b.push(scaling({1.0f, 0.85f, 0.85f}));
        b.icosphere({}, 0.45f, 1, 0.08f, 17);
        b.pop();
    });
    b.color(hive);
    b.cylinder({0.4f, 0, 1.0f}, {1.2f, 0, 1.5f}, 0.13f, 6);
    b.cone({1.2f, 0, 1.5f}, {1.35f, 0, 1.6f}, 0.18f, 6);
    b.color(chitin);
    legs(b, 0.35f, 0.6f, 0.6f, 0.06f, 2, 0.2f, -0.6f);
}

void boat(Builder& b) {
    Rings h;
    auto vring = [](float x, float w, float deck, float keel) {
        return std::vector<Vec3>{{x, 0, keel},
                                 {x, w * 0.75f, keel + (deck - keel) * 0.35f},
                                 {x, w, deck},
                                 {x, -w, deck},
                                 {x, -w * 0.75f, keel + (deck - keel) * 0.35f}};
    };
    h.push_back(vring(4.6f, 0.05f, 1.3f, 1.1f));
    h.push_back(vring(3.5f, 1.0f, 1.25f, 0.2f));
    h.push_back(vring(0.5f, 1.45f, 1.2f, 0.0f));
    h.push_back(vring(-4.0f, 1.4f, 1.15f, 0.15f));
    b.color(white);
    b.loft(h);
    b.team();
    b.mirrored([&] { b.box({0.3f, 1.44f, 0.95f}, {7.0f, 0.04f, 0.2f}); });
    b.color(bone);
    b.wedgeBox({-0.2f, 0, 1.75f}, {2.4f, 1.8f, 1.1f}, 0.35f, 0.1f, -0.2f);
    b.color(glass);
    b.box({0.9f, 0, 1.9f}, {0.2f, 1.6f, 0.5f});
    b.color(gunmetal);
    b.box({-4.2f, 0, 1.1f}, {0.5f, 0.6f, 0.9f});
    b.cylinder({-0.4f, 0, 2.3f}, {-0.4f, 0, 3.6f}, 0.05f, 5);
    b.color(lamp, Material::Emissive);
    b.icosphere({-0.4f, 0, 3.65f}, 0.07f, 0);
}

void buoy(Builder& b) {
    b.color(hex(0xD8432F));
    verticalLathe(b, std::array<Vec2, 4>{{{-0.4f, 0.0f}, {-0.3f, 0.7f}, {0.6f, 0.7f}, {0.7f, 0.0f}}}, 8);
    b.team();
    verticalLathe(b, std::array<Vec2, 2>{{{0.3f, 0.71f}, {0.15f, 0.71f}}}, 8);
    b.color(gunmetal);
    for (int i = 0; i < 3; ++i) {
        const float a = F(i) * 2 * kPi / 3;
        b.cylinder({0.5f * std::cos(a), 0.5f * std::sin(a), 0.6f}, {0, 0, 2.0f}, 0.03f, 4);
    }
    b.color(lamp, Material::Emissive);
    b.icosphere({0, 0, 2.05f}, 0.12f, 1);
}

void groundStation(Builder& b) {
    b.color(bone);
    b.box({0, 0, 0.6f}, {2.0f, 2.0f, 1.2f});
    b.color(steel);
    b.cylinder({0, 0, 1.2f}, {0, 0, 2.6f}, 0.25f, 8);
    b.at({0, 0, 3.0f}, axisAngle({0, 1, 0}, -0.7f), [&] {
        b.color(white);
        b.lathe(std::array<Vec2, 4>{{{-0.2f, 0.0f}, {0.0f, 0.6f}, {0.35f, 1.7f}, {0.45f, 1.9f}}}, 14);
        b.color(steel);
        for (int i = 0; i < 3; ++i) {
            const float a = F(i) * 2 * kPi / 3;
            b.cylinder({0.4f, 1.7f * std::cos(a), 1.7f * std::sin(a)}, {1.6f, 0, 0}, 0.025f, 4);
        }
        b.team();
        b.cylinder({1.5f, 0, 0}, {1.75f, 0, 0}, 0.1f, 6);
    });
}

void tower(Builder& b) {
    b.color(hex(0xC8C8C2));
    const float h = 30.0f;
    for (int i = 0; i < 4; ++i) {
        const float a = kPi / 4 + F(i) * kPi / 2, a1 = a + kPi / 2;
        const Vec3 base{2.2f * std::cos(a), 2.2f * std::sin(a), 0}, top{0.5f * std::cos(a), 0.5f * std::sin(a), h};
        const Vec3 base1{2.2f * std::cos(a1), 2.2f * std::sin(a1), 0}, top1{0.5f * std::cos(a1), 0.5f * std::sin(a1), h};
        b.cylinder(base, top, 0.09f, 4);
        for (int k = 0; k < 6; ++k) {
            const float t0 = F(k) / 6.0f, t1 = F(k + 1) / 6.0f;
            b.cylinder(lerp(base, top, t0), lerp(base1, top1, t1), 0.04f, 4);
        }
    }
    b.color(gunmetal);
    b.box({0, 0, h}, {1.4f, 1.4f, 0.2f});
    b.team();
    b.cylinder({0, 0, h}, {0, 0, h + 4.0f}, 0.08f, 5);
    b.color(bone);
    for (float z : {h - 2.0f, h - 5.0f}) b.box({0.65f, 0, z}, {0.15f, 0.5f, 1.6f});
    b.color(navRed, Material::Emissive);
    b.icosphere({0, 0, h + 4.1f}, 0.18f, 1);
}

void building(Builder& b) {
    b.color(sand);
    b.box({0, 0, 4.5f}, {12.0f, 10.0f, 9.0f});
    b.color(dune);
    b.box({0, 0, 9.2f}, {12.2f, 10.2f, 0.4f});
    b.box({0, 0, 0.3f}, {12.3f, 10.3f, 0.6f});
    b.color(lamp, Material::Emissive);
    for (int f = 0; f < 3; ++f) {
        const float z = 1.8f + F(f) * 2.7f;
        b.mirrored([&] {
            for (int w = 0; w < 4; ++w) b.box({-4.2f + F(w) * 2.8f, 5.01f, z}, {1.2f, 0.04f, 0.9f});
        });
    }
    b.color(glass);
    b.box({6.01f, 0, 1.5f}, {0.04f, 2.4f, 2.6f});
    b.color(steel);
    b.box({2.0f, 2.0f, 9.9f}, {2.0f, 1.4f, 1.0f});
    b.box({-3.0f, -2.0f, 9.7f}, {1.2f, 1.2f, 0.6f});
    b.team();
    b.box({0, 0, 9.42f}, {12.22f, 10.22f, 0.05f});
}

void tree(Builder& b) {
    b.color(bark);
    b.cylinder({0, 0, 0}, {0, 0, 2.2f}, 0.22f, 6);
    const float tiers[3][3] = {{1.6f, 2.4f, 4.5f}, {3.3f, 1.9f, 3.6f}, {4.9f, 1.3f, 3.0f}};
    for (int i = 0; i < 3; ++i) {
        b.color(i % 2 ? hex(0x4A7A42) : leaf);
        b.cone({0, 0, tiers[i][0]}, {0, 0, tiers[i][0] + tiers[i][2]}, tiers[i][1], 7);
    }
}

void palm(Builder& b) {
    b.color(bark);
    Vec3 p{0, 0, 0};
    for (int i = 0; i < 6; ++i) {
        const Vec3 q{0.12f * F(i + 1) * F(i + 1) * 0.06f + 0.05f * F(i + 1), 0, 1.4f * F(i + 1)};
        b.cylinder(p, q, 0.24f - 0.015f * F(i), 6);
        p = q;
    }
    b.color(frond);
    for (int i = 0; i < 8; ++i) {
        const float a = F(i) * 2 * kPi / 8 + 0.2f;
        const Vec3 d{std::cos(a), std::sin(a), 0}, side{-std::sin(a), std::cos(a), 0};
        const Vec3 mid = p + d * 1.8f + Vec3{0, 0, 0.6f}, tip = p + d * 3.6f + Vec3{0, 0, -0.6f};
        b.panel(p, mid + side * 0.45f, mid, p);
        b.panel(p, mid, mid - side * 0.45f, p);
        b.panel(mid + side * 0.45f, tip, mid, mid);
        b.panel(mid, tip, mid - side * 0.45f, mid);
    }
    b.color(hex(0x6B4A2A));
    b.icosphere(p + Vec3{0.2f, 0, -0.2f}, 0.22f, 0);
}

void rock(Builder& b) {
    b.color(basalt);
    b.push(scaling({2.2f, 1.6f, 1.0f}));
    b.icosphere({0, 0, 0.5f}, 1.0f, 1, 0.22f, 29);
    b.pop();
}

void crate(Builder& b) {
    b.color(khaki);
    b.box({0, 0, 0.6f}, {1.2f, 1.2f, 1.2f});
    b.color(hex(0x6E6444));
    for (float y : {-0.61f, 0.61f}) {
        b.box({0, y, 0.6f}, {1.22f, 0.03f, 0.14f});
        b.box({0, y, 0.6f}, {0.14f, 0.03f, 1.22f});
    }
    b.team();
    b.box({0.61f, 0, 0.9f}, {0.03f, 0.5f, 0.2f});
}

void marker(Builder& b) {
    b.color(bone, Material::TeamEmissive);
    const Vec3 top{0, 0, 1.6f}, bottom{0, 0, 0.4f}, c{0, 0, 1.15f};
    const Vec3 ring4[4] = {{0.35f, 0, 1.15f}, {0, 0.35f, 1.15f}, {-0.35f, 0, 1.15f}, {0, -0.35f, 1.15f}};
    for (int i = 0; i < 4; ++i) {
        b.tri(ring4[i], ring4[(i + 1) % 4], top, c);
        b.tri(ring4[i], ring4[(i + 1) % 4], bottom, c);
    }
    b.color(gunmetal);
    b.cylinder({0, 0, 0}, {0, 0, 0.35f}, 0.03f, 4);
    verticalLathe(b, std::array<Vec2, 2>{{{0.02f, 0.3f}, {0.0f, 0.3f}}}, 12);
}

void node(Builder& b) {
    // A monolith in the proportions 1:4:9, lit along its seams.
    b.color(hex(0x111317));
    b.box({0, 0, 1.8f}, {0.4f, 1.6f, 3.6f});
    b.color(bone, Material::TeamEmissive);
    b.mirrored([&] {
        b.box({0.0f, 0.805f, 1.8f}, {0.42f, 0.02f, 3.62f});
        b.box({0.205f, 0.4f, 1.8f}, {0.02f, 0.02f, 3.4f});
    });
    b.box({0.205f, 0, 3.3f}, {0.02f, 1.2f, 0.02f});
    b.box({0.205f, 0, 0.3f}, {0.02f, 1.2f, 0.02f});
    b.color(graphite);
    b.box({0, 0, 0.03f}, {1.2f, 2.4f, 0.06f});
}

void operatorBust(Builder& b) {
    b.color(hex(0x111317));
    b.wedgeBox({-0.02f, 0, -0.36f}, {0.32f, 0.56f, 0.26f}, 0.06f, 0.16f);
    b.cylinder({0, 0, -0.26f}, {0, 0, -0.12f}, 0.06f, 6);
    b.color(gunmetal);
    b.push(scaling({1.0f, 0.82f, 1.15f}));
    b.icosphere({0, 0, 0.0f}, 0.11f, 1);
    b.pop();
    b.color(bone, Material::TeamEmissive);
    b.wedgeBox({0.085f, 0, 0.015f}, {0.05f, 0.2f, 0.045f}, 0.01f, 0.02f);
    b.color(hex(0x111317));
    b.cylinder({-0.01f, -0.095f, -0.02f}, {-0.03f, -0.1f, -0.2f}, 0.006f, 4);
}

void wand(Builder& b) {
    // A slim controller, aim axis along -z in the render frame: here along +x (forward), origin at the grip.
    b.color(hex(0x1A1D22));
    b.lathe(std::array<Vec2, 5>{{{0.06f, 0.0f}, {0.05f, 0.014f}, {-0.02f, 0.017f}, {-0.07f, 0.015f}, {-0.08f, 0.0f}}}, 10);
    b.color(hex(0x2A2E35));
    b.at({0.045f, 0, 0.012f}, axisAngle({0, 1, 0}, 0.35f),
         [&] { b.lathe(std::array<Vec2, 4>{{{0.02f, 0.0f}, {0.02f, 0.034f}, {-0.012f, 0.034f}, {-0.012f, 0.0f}}}, 14); });
    b.color(bone, Material::TeamEmissive);
    b.at({0.045f, 0, 0.012f}, axisAngle({0, 1, 0}, 0.35f), [&] { b.lathe(std::array<Vec2, 2>{{{0.021f, 0.035f}, {0.017f, 0.035f}}}, 14); });
    b.cylinder({0.061f, 0, 0}, {0.064f, 0, 0}, 0.005f, 8);
}

} // namespace

Mesh build(Shape s) {
    Builder b;
    switch (s) {
    case Shape::Kite: kite(b); break;
    case Shape::Quadcopter: quadcopter(b); break;
    case Shape::FixedWing: fixedWing(b); break;
    case Shape::Fighter: fighter(b, hex(0x8A939C)); break;
    case Shape::Striker: striker(b); break;
    case Shape::Helicopter: helicopter(b); break;
    case Shape::Interceptor: interceptor(b); break;
    case Shape::Missile: missile(b); break;
    case Shape::Bomb: bomb(b); break;
    case Shape::RelaySat: relaySat(b); break;
    case Shape::ImagingSat: imagingSat(b); break;
    case Shape::Truck: truck(b); break;
    case Shape::Tank: tank(b); break;
    case Shape::Apc: apc(b); break;
    case Shape::SamLauncher: samLauncher(b); break;
    case Shape::AaaGun: aaaGun(b); break;
    case Shape::Radar: radar(b); break;
    case Shape::AcousticArray: acousticArray(b); break;
    case Shape::Jammer: jammer(b); break;
    case Shape::Rover: rover(b); break;
    case Shape::RobotArm: robotArm(b); break;
    case Shape::Bunker: bunker(b); break;
    case Shape::Turret: turret(b); break;
    case Shape::Swarmer: swarmer(b); break;
    case Shape::Bruiser: bruiser(b); break;
    case Shape::Spitter: spitter(b); break;
    case Shape::Boat: boat(b); break;
    case Shape::Buoy: buoy(b); break;
    case Shape::GroundStation: groundStation(b); break;
    case Shape::Tower: tower(b); break;
    case Shape::Building: building(b); break;
    case Shape::Tree: tree(b); break;
    case Shape::Palm: palm(b); break;
    case Shape::Rock: rock(b); break;
    case Shape::Crate: crate(b); break;
    case Shape::Marker: marker(b); break;
    case Shape::Node: node(b); break;
    case Shape::Operator: operatorBust(b); break;
    case Shape::Wand: wand(b); break;
    case Shape::Count: break;
    }
    return b.finish();
}

namespace {
constexpr std::array<std::string_view, static_cast<std::size_t>(Shape::Count)> kNames{
    "kite",    "quadcopter",     "fixed_wing",  "fighter", "striker",        "helicopter", "interceptor",  "missile",
    "bomb",    "relay_sat",      "imaging_sat", "truck",   "tank",           "apc",        "sam_launcher", "aaa_gun",
    "radar",   "acoustic_array", "jammer",      "rover",   "robot_arm",      "bunker",     "turret",       "swarmer",
    "bruiser", "spitter",        "boat",        "buoy",    "ground_station", "tower",      "building",     "tree",
    "palm",    "rock",           "crate",       "marker",  "node",           "operator",   "wand"};

constexpr std::array<Shape, static_cast<std::size_t>(Shape::Count)> makeAll() {
    std::array<Shape, static_cast<std::size_t>(Shape::Count)> a{};
    for (std::size_t i = 0; i < a.size(); ++i) a[i] = static_cast<Shape>(i);
    return a;
}
constexpr auto kAll = makeAll();
} // namespace

std::string_view name(Shape s) {
    const auto i = static_cast<std::size_t>(s);
    return i < kNames.size() ? kNames[i] : std::string_view{"unknown"};
}

Shape shapeFromName(std::string_view n, Shape fallback) {
    for (std::size_t i = 0; i < kNames.size(); ++i)
        if (kNames[i] == n) return static_cast<Shape>(i);
    return fallback;
}

std::span<const Shape> allShapes() {
    return kAll;
}

Domain domain(Shape s) {
    switch (s) {
    case Shape::Kite:
    case Shape::Quadcopter:
    case Shape::FixedWing:
    case Shape::Fighter:
    case Shape::Striker:
    case Shape::Helicopter:
    case Shape::Interceptor:
    case Shape::Missile:
    case Shape::Bomb: return Domain::Air;
    case Shape::RelaySat:
    case Shape::ImagingSat: return Domain::Space;
    case Shape::Boat:
    case Shape::Buoy: return Domain::Sea;
    case Shape::GroundStation:
    case Shape::Tower:
    case Shape::Building:
    case Shape::Tree:
    case Shape::Palm:
    case Shape::Rock:
    case Shape::Crate: return Domain::Structure;
    case Shape::Marker:
    case Shape::Node:
    case Shape::Operator:
    case Shape::Wand: return Domain::Glyph;
    default: return Domain::Ground;
    }
}

Vec3 eyepoint(Shape s) {
    switch (s) {
    case Shape::Kite: return {0.34f, 0, -0.03f};
    case Shape::Quadcopter: return {0.13f, 0, -0.03f};
    case Shape::FixedWing: return {0.7f, 0, -0.08f};
    case Shape::Fighter: return {4.0f, 0, 1.05f};
    case Shape::Striker: return {3.6f, 0, 1.1f};
    case Shape::Helicopter: return {2.3f, 0.45f, 0.75f};
    case Shape::Interceptor: return {0.9f, 0, 0.12f};
    case Shape::Missile: return {1.25f, 0, 0};
    case Shape::Bomb: return {1.3f, 0, 0};
    case Shape::RelaySat: return {0.8f, 0, 0};
    case Shape::ImagingSat: return {1.7f, 0, 0};
    case Shape::Truck:
    case Shape::SamLauncher:
    case Shape::Jammer: return {3.1f, 0.55f, 2.25f};
    case Shape::Tank: return {-0.9f, 0.6f, 2.95f};
    case Shape::Apc: return {3.1f, 0.6f, 2.3f};
    case Shape::AaaGun: return {0.2f, 0, 2.1f};
    case Shape::Radar: return {0.3f, 0, 4.4f};
    case Shape::AcousticArray: return {0.2f, 0, 4.4f};
    case Shape::Rover: return {0.74f, 0.3f, 1.63f};
    case Shape::RobotArm: return {1.7f, 0, 2.3f};
    case Shape::Bunker: return {2.6f, 0, 3.6f};
    case Shape::Turret: return {0.9f, 0, 1.75f};
    case Shape::Swarmer: return {0.75f, 0, 0.62f};
    case Shape::Bruiser: return {2.4f, 0, 1.6f};
    case Shape::Spitter: return {1.3f, 0, 1.6f};
    case Shape::Boat: return {0.6f, 0.4f, 2.4f};
    case Shape::Buoy: return {0, 0, 2.3f};
    case Shape::GroundStation: return {1.2f, 0, 3.6f};
    case Shape::Tower: return {0.8f, 0, 30.4f};
    case Shape::Building: return {0, 0, 10.6f};
    case Shape::Tree: return {0, 0, 8.6f};
    case Shape::Palm: return {0, 0, 8.6f};
    case Shape::Rock: return {0, 0, 1.6f};
    case Shape::Crate: return {0, 0, 1.4f};
    case Shape::Marker: return {0, 0, 1.8f};
    case Shape::Node: return {0.3f, 0, 3.8f};
    case Shape::Operator: return {0.1f, 0, 0.02f};
    case Shape::Wand: return {0.06f, 0, 0.02f};
    case Shape::Count: break;
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Terrain

Mesh terrain(const HeightSamples& f, int step, float clipRadius, float cx, float cy) {
    Builder b;
    step = std::max(step, 1);
    const int nx = (f.w - 1) / step, ny = (f.h - 1) / step;
    if (nx < 1 || ny < 1 || !f.z) return b.finish();
    auto P = [&](int i, int j) {
        const int si = std::min(i * step, f.w - 1), sj = std::min(j * step, f.h - 1);
        return Vec3{f.x0 + F(si) * f.cell, f.y0 + F(sj) * f.cell,
                    f.z[static_cast<std::size_t>(sj) * static_cast<std::size_t>(f.w) + static_cast<std::size_t>(si)]};
    };
    float lo = 1e30f, hi = -1e30f;
    for (int k = 0; k < f.w * f.h; ++k) {
        lo = std::min(lo, f.z[k]);
        hi = std::max(hi, f.z[k]);
    }
    const float span = std::max(hi - lo, 1.0f);
    const Rgb8 low = hex(0x8E7650), mid = hex(0xB79A6B), high = hex(0xD9C49A), rockC = hex(0x6E6355);
    auto mix = [](Rgb8 a, Rgb8 c, float t) {
        t = clamp(t, 0.0f, 1.0f);
        return Rgb8{static_cast<std::uint8_t>(F(a.r) + (F(c.r) - F(a.r)) * t), static_cast<std::uint8_t>(F(a.g) + (F(c.g) - F(a.g)) * t),
                    static_cast<std::uint8_t>(F(a.b) + (F(c.b) - F(a.b)) * t)};
    };
    auto face = [&](Vec3 a, Vec3 c, Vec3 d) {
        const Vec3 n = normalize(cross(c - a, d - a));
        const float h = ((a.z + c.z + d.z) / 3.0f - lo) / span;
        const float slope = 1.0f - std::fabs(n.z);
        Rgb8 col = h < 0.5f ? mix(low, mid, h * 2.0f) : mix(mid, high, (h - 0.5f) * 2.0f);
        col = mix(col, rockC, smoothstep(0.12f, 0.35f, slope));
        if (clipRadius > 0) {
            const Vec3 m = (a + c + d) / 3.0f;
            const float r = std::sqrt((m.x - cx) * (m.x - cx) + (m.y - cy) * (m.y - cy)) / clipRadius;
            col = mix(col, Rgb8{18, 16, 14}, smoothstep(0.72f, 1.0f, r));
        }
        b.color(col);
        // Inside is below the surface.
        b.tri(a, c, d, (a + c + d) / 3.0f - Vec3{0, 0, 10.0f});
    };
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const Vec3 a = P(i, j), c = P(i + 1, j), d = P(i + 1, j + 1), e = P(i, j + 1);
            if (clipRadius > 0) {
                const float mx = (a.x + d.x) * 0.5f - cx, my = (a.y + d.y) * 0.5f - cy;
                if (mx * mx + my * my > clipRadius * clipRadius) continue;
            }
            // Alternate the diagonal for a less regular, more faceted look.
            if ((i + j) % 2) {
                face(a, c, d);
                face(a, d, e);
            } else {
                face(a, c, e);
                face(c, d, e);
            }
        }
    return b.finish();
}

// ---------------------------------------------------------------------------------------------
// glTF export

namespace {

float srgbToLinear(std::uint8_t c) {
    const float v = F(c) / 255.0f;
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

void append(std::vector<std::uint8_t>& out, const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}

std::string num(float v) {
    char buf[32];
    const int n = std::snprintf(buf, sizeof buf, "%.6g", static_cast<double>(v));
    return std::string(buf, static_cast<std::size_t>(n));
}

} // namespace

std::vector<std::uint8_t> exportGlb() {
    std::vector<std::uint8_t> bin;
    std::string meshes, accessors, views, nodes, sceneNodes;
    int accessor = 0, view = 0;
    auto addView = [&](const void* data, std::size_t bytes, int target) {
        while (bin.size() % 4) bin.push_back(0);
        const std::size_t offset = bin.size();
        append(bin, data, bytes);
        if (!views.empty()) views += ",";
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(bytes) +
                 ",\"target\":" + std::to_string(target) + "}";
        return view++;
    };
    auto addAccessor = [&](int v, int componentType, std::size_t count, const char* type, const std::string& extra) {
        if (!accessors.empty()) accessors += ",";
        accessors += "{\"bufferView\":" + std::to_string(v) + ",\"componentType\":" + std::to_string(componentType) +
                     ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" + extra + "}";
        return accessor++;
    };

    int index = 0;
    for (Shape s : allShapes()) {
        const Mesh m = build(s);
        std::string prims;
        for (int code = 0; code < 4; ++code) {
            std::vector<float> pos, nrm, col;
            Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            for (const Vertex& v : m.vertices) {
                if (v.a / 64 != code) continue;
                // FLU to glTF (+y up, +z forward, +x left).
                const Vec3 p{v.py, v.pz, v.px};
                pos.insert(pos.end(), {p.x, p.y, p.z});
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                const Vec3 n = normalize(Vec3{F(v.ny), F(v.nz), F(v.nx)});
                nrm.insert(nrm.end(), {n.x, n.y, n.z});
                col.insert(col.end(), {srgbToLinear(v.r), srgbToLinear(v.g), srgbToLinear(v.b)});
            }
            if (pos.empty()) continue;
            const std::size_t count = pos.size() / 3;
            const int pa = addAccessor(addView(pos.data(), pos.size() * 4, 34962), 5126, count, "VEC3",
                                       ",\"min\":[" + num(lo.x) + "," + num(lo.y) + "," + num(lo.z) + "],\"max\":[" + num(hi.x) + "," +
                                           num(hi.y) + "," + num(hi.z) + "]");
            const int na = addAccessor(addView(nrm.data(), nrm.size() * 4, 34962), 5126, count, "VEC3", "");
            const int ca = addAccessor(addView(col.data(), col.size() * 4, 34962), 5126, count, "VEC3", "");
            if (!prims.empty()) prims += ",";
            prims += "{\"attributes\":{\"POSITION\":" + std::to_string(pa) + ",\"NORMAL\":" + std::to_string(na) +
                     ",\"COLOR_0\":" + std::to_string(ca) + "},\"material\":" + std::to_string(code) + "}";
        }
        if (!meshes.empty()) meshes += ",";
        meshes += "{\"name\":\"" + std::string(name(s)) + "\",\"primitives\":[" + prims + "]}";
        if (!nodes.empty()) nodes += ",";
        // Lay the kit out on a grid in the default scene; consumers look meshes up by name.
        const float gx = F(index % 8) * 16.0f, gz = F(index / 8) * 16.0f;
        nodes += "{\"name\":\"" + std::string(name(s)) + "\",\"mesh\":" + std::to_string(index) + ",\"translation\":[" + num(gx) + ",0," +
                 num(gz) + "]}";
        if (!sceneNodes.empty()) sceneNodes += ",";
        sceneNodes += std::to_string(index);
        ++index;
    }

    const std::string materials = "{\"name\":\"matte\",\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":0.85}},"
                                  "{\"name\":\"team\",\"pbrMetallicRoughness\":{\"metallicFactor\":0.1,\"roughnessFactor\":0.6}},"
                                  "{\"name\":\"emissive\",\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1},"
                                  "\"extensions\":{\"KHR_materials_unlit\":{}}},"
                                  "{\"name\":\"team_emissive\",\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1},"
                                  "\"extensions\":{\"KHR_materials_unlit\":{}}}";
    while (bin.size() % 4) bin.push_back(0);
    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"glyder kit\"},\"extensionsUsed\":[\"KHR_materials_unlit\"],"
                       "\"scene\":0,\"scenes\":[{\"name\":\"kit\",\"nodes\":[" +
                       sceneNodes + "]}],\"nodes\":[" + nodes + "],\"meshes\":[" + meshes + "],\"materials\":[" + materials +
                       "],\"accessors\":[" + accessors + "],\"bufferViews\":[" + views +
                       "],\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]}";
    while (json.size() % 4) json += ' ';

    std::vector<std::uint8_t> out;
    auto u32 = [&](std::uint32_t v) { append(out, &v, 4); };
    u32(0x46546C67u); // "glTF"
    u32(2);
    u32(static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
    u32(static_cast<std::uint32_t>(json.size()));
    u32(0x4E4F534Au); // "JSON"
    append(out, json.data(), json.size());
    u32(static_cast<std::uint32_t>(bin.size()));
    u32(0x004E4942u); // "BIN"
    append(out, bin.data(), bin.size());
    return out;
}

} // namespace glyder::kit
