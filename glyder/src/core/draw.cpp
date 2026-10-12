#include <glyder/draw.hpp>

#include <array>
#include <cctype>
#include <cmath>
#include <string_view>

namespace glyder::render {

void addPanel(std::vector<PanelVertex>& out, Vec3 c, Vec3 right, Vec3 up, Vec2 half, const PanelStyle& s) {
    // Grow the quad by the glow so the halo has room; the shader works in true panel coordinates.
    const float pad = s.glow > 0 ? s.glow * 4.0f + s.softness : 0.0f;
    const float hx = half.x + pad, hy = half.y + pad;
    const Vec4 shape{s.radius, s.border, s.softness, s.glow};
    auto v = [&](float x, float y) {
        PanelVertex p;
        p.pos = c + right * x + up * y;
        p.local = {x, y, half.x, half.y};
        p.shape = shape;
        p.fill[0] = s.fill.r;
        p.fill[1] = s.fill.g;
        p.fill[2] = s.fill.b;
        p.fill[3] = s.fillAlpha;
        p.border[0] = s.borderColour.r;
        p.border[1] = s.borderColour.g;
        p.border[2] = s.borderColour.b;
        p.border[3] = s.borderAlpha;
        out.push_back(p);
    };
    v(-hx, -hy);
    v(hx, -hy);
    v(hx, hy);
    v(-hx, -hy);
    v(hx, hy);
    v(-hx, hy);
}

ViewData viewData(const Mat4& view, const Mat4& proj) {
    ViewData v;
    v.viewProj = proj * view;
    v.invViewProj = inverse(v.viewProj);
    const Mat4 inv = rigidInverse(view);
    v.eye = {inv.m[3][0], inv.m[3][1], inv.m[3][2], 1.0f};
    return v;
}

namespace {
Vec3 lin(std::uint32_t c) {
    auto f = [](std::uint32_t v) {
        const float x = static_cast<float>(v & 0xFFu) / 255.0f;
        return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
    };
    return {f(c >> 16), f(c >> 8), f(c)};
}
Vec4 v4(Vec3 v, float w) {
    return {v.x, v.y, v.z, w};
}
Vec3 dirFromAngles(float azimuthDeg, float elevationDeg) {
    // Azimuth clockwise from north, in the render frame (north is -z).
    const float az = azimuthDeg * kDeg, el = elevationDeg * kDeg;
    return toRender({std::sin(az) * std::cos(el), std::cos(az) * std::cos(el), std::sin(el)});
}
} // namespace

void applyEnvironment(SceneUniforms& s, Environment e, float renderPerWorld, float groundRenderY) {
    const float k = renderPerWorld;
    switch (e) {
    case Environment::Night: {
        const Vec3 moon = dirFromAngles(215.0f, 38.0f);
        s.sunDir = v4(moon, 1.0f);
        s.sunColor = v4(Vec3{0.22f, 0.27f, 0.38f}, 0.25f);
        s.skyZenith = v4(lin(0x03060F), 1.0f);
        s.skyHorizon = v4(lin(0x1A2238), 0.000045f / k);
        s.groundColor = v4(lin(0x060608), s.groundColor.w);
        s.moon = v4(moon, 0.012f);
        s.contour = {50.0f, 0.55f, groundRenderY, k};
        s.contourColor = v4(lin(0x3E6C9A), 0.0f);
        s.ambientSky = v4(Vec3{0.035f, 0.045f, 0.075f}, 1.0f);
        s.ambientGround = v4(Vec3{0.012f, 0.011f, 0.012f}, 0.0f);
        break;
    }
    case Environment::Dusk: {
        const Vec3 sun = dirFromAngles(255.0f, 6.0f);
        s.sunDir = v4(sun, 1.0f);
        s.sunColor = v4(Vec3{1.25f, 0.62f, 0.32f}, 0.3f);
        s.skyZenith = v4(lin(0x1B2A55), 0.15f);
        s.skyHorizon = v4(lin(0xE07A45), 0.00004f / k);
        s.groundColor = v4(lin(0x22191A), s.groundColor.w);
        s.moon = v4(sun, 0.02f);
        s.contour = {50.0f, 0.18f, groundRenderY, k};
        s.contourColor = v4(lin(0xFFB27A), 0.0f);
        s.ambientSky = v4(Vec3{0.12f, 0.12f, 0.2f}, 1.0f);
        s.ambientGround = v4(Vec3{0.06f, 0.04f, 0.035f}, 0.0f);
        break;
    }
    case Environment::Day: {
        const Vec3 sun = dirFromAngles(160.0f, 55.0f);
        s.sunDir = v4(sun, 1.0f);
        s.sunColor = v4(Vec3{1.9f, 1.8f, 1.6f}, 0.12f);
        s.skyZenith = v4(lin(0x3C78C8), 0.0f);
        s.skyHorizon = v4(lin(0xBFD4E6), 0.00002f / k);
        s.groundColor = v4(lin(0x5A5040), s.groundColor.w);
        s.moon = v4(sun, 0.0f);
        s.contour = {50.0f, 0.08f, groundRenderY, k};
        s.contourColor = v4(lin(0x40362A), 0.0f);
        s.ambientSky = v4(Vec3{0.32f, 0.38f, 0.48f}, 1.0f);
        s.ambientGround = v4(Vec3{0.14f, 0.12f, 0.09f}, 0.0f);
        break;
    }
    case Environment::Construct: {
        // The white void: shadowless, endless, every object a crisp graphite silhouette.
        const Vec3 key = dirFromAngles(140.0f, 50.0f);
        s.sunDir = v4(key, 0.55f);
        s.sunColor = v4(Vec3{1.0f, 1.0f, 1.0f}, 0.0f);
        s.skyZenith = v4(lin(0xE9EBEE), 0.0f);
        s.skyHorizon = v4(lin(0xF7F7F5), 0.004f / k);
        s.groundColor = v4(lin(0xE3E5E8), s.groundColor.w);
        s.moon = v4(key, 0.0f);
        s.contour = {1.0f, 0.0f, groundRenderY, k};
        s.contourColor = v4(lin(0x9FA4AA), 0.0f);
        s.ambientSky = v4(Vec3{0.62f, 0.63f, 0.65f}, 1.0f);
        s.ambientGround = v4(Vec3{0.3f, 0.3f, 0.31f}, 0.0f);
        break;
    }
    case Environment::Studio: {
        const Vec3 key = dirFromAngles(120.0f, 42.0f);
        s.sunDir = v4(key, 1.15f);
        s.sunColor = v4(Vec3{1.0f, 0.97f, 0.92f}, 0.55f);
        s.skyZenith = v4(lin(0x0C0E12), 0.0f);
        s.skyHorizon = v4(lin(0x23272E), 0.0f);
        s.groundColor = v4(lin(0x16181B), s.groundColor.w);
        s.moon = v4(key, 0.0f);
        s.contour = {1.0f, 0.0f, groundRenderY, k};
        s.contourColor = v4(lin(0x000000), 0.0f);
        s.ambientSky = v4(Vec3{0.30f, 0.32f, 0.36f}, 1.0f);
        s.ambientGround = v4(Vec3{0.07f, 0.065f, 0.06f}, 0.0f);
        break;
    }
    }
}

// ---------------------------------------------------------------------------------------------
// The stroke font. Each glyph is a set of polylines on an 8 x 12 grid (y up, baseline 0). A point
// is two characters, x then y, each 0-9 or a-c for 10-12; 'z' is -1 and 'y' -2 for descenders.
// Polylines are separated by spaces.

namespace {

struct Glyph {
    char c;
    const char* strokes;
};

constexpr Glyph kGlyphs[] = {
    {'A', "004c80 1373"},
    {'B', "000c6c8a886606 6684826000"},
    {'C', "8a6c2c0a02206082"},
    {'D', "000c5c89835000"},
    {'E', "8c0c0080 0666"},
    {'F', "8c0c00 0666"},
    {'G', "8a6c2c0a022060828656"},
    {'H', "000c 808c 0686"},
    {'I', "2c6c 4c40 2060"},
    {'J', "8c8260200204"},
    {'K', "000c 048c 3780"},
    {'L', "0c0080"},
    {'M', "000c468c80"},
    {'N', "000c808c"},
    {'O', "2003092c6c89836020"},
    {'P', "000c6c8a876505"},
    {'Q', "2003092c6c89836020 5380"},
    {'R', "000c6c8a876505 4580"},
    {'S', "8a6c2c0a0826668482602002"},
    {'T', "0c8c 4c40"},
    {'U', "0c022060828c"},
    {'V', "0c408c"},
    {'W', "0c2048608c"},
    {'X', "008c 0c80"},
    {'Y', "0c46 8c46 4640"},
    {'Z', "0c8c0080"},
    {'0', "2003092c6c89836020 226a"},
    {'1', "2a4c40 2060"},
    {'2', "0a2c6c8a880080"},
    {'3', "0a2c6c8a886636 668482602002"},
    {'4', "606c0484"},
    {'5', "8c0c07678582602002"},
    {'6', "7c3c0902206082856707"},
    {'7', "0c8c30"},
    {'8', "26080a2c6c8a886626 2604022060828466"},
    {'9', "8525070a2c6c8a826010"},
    {'.', "4041"},
    {',', "42413z"},
    {':', "4344 4849"},
    {';', "4849 42413z"},
    {'-', "1676"},
    {'+', "1676 4349"},
    {'/', "107c"},
    {'\\', "1c70"},
    {'(', "5c3a3250"},
    {')', "3c5a5230"},
    {'[', "5c3c3050"},
    {']', "3c5c5030"},
    {'<', "7a1672"},
    {'>', "1a7612"},
    {'=', "1474 1878"},
    {'_', "0080"},
    {'!', "4c44 4041"},
    {'?', "0a2c6c8a88664644 4041"},
    {'\'', "4c4a"},
    {'"', "3c3a 5c5a"},
    {'#', "2c20 6c60 0484 0888"},
    {'*', "4a42 1874 1478"},
    {'|', "4c40"},
    {'%', "107c 1a2b1c0b1a 7081726170"},
    {'^', "2a4c6a"},
    {'~', "06283648"},
    {'@', "6454242818a8ac6c4c0a02206082"},
    {'&', "8030090b2c4c5a5808 0802206084"},
};

int coord(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'c') return 10 + (c - 'a');
    if (c == 'z') return -1;
    if (c == 'y') return -2;
    return 0;
}

const char* strokesFor(char c) {
    const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const Glyph& g : kGlyphs)
        if (g.c == u) return g.strokes;
    return nullptr;
}

constexpr float kAdvance = 11.0f; // grid units per character, including spacing
constexpr float kCap = 12.0f;

} // namespace

float textWidth(std::string_view s, float height) {
    if (s.empty()) return 0.0f;
    return (static_cast<float>(s.size()) * kAdvance - 3.0f) * height / kCap;
}

void text(std::vector<Line>& out, std::string_view s, Vec3 origin, Vec3 right, Vec3 up, float height, Rgb8 colour, Align align,
          float weight, std::uint8_t alpha) {
    const float unit = height / kCap;
    const float w = textWidth(s, height);
    if (align == Align::Centre) origin -= right * (w * 0.5f);
    if (align == Align::Right) origin -= right * w;
    const float stroke = height * weight;
    float x0 = 0;
    for (char c : s) {
        if (const char* g = strokesFor(c)) {
            std::string_view rest(g);
            while (!rest.empty()) {
                const std::size_t sp = rest.find(' ');
                const std::string_view poly = rest.substr(0, sp);
                rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp + 1);
                for (std::size_t i = 0; i + 3 < poly.size(); i += 2) {
                    const float ax = x0 + static_cast<float>(coord(poly[i])), ay = static_cast<float>(coord(poly[i + 1]));
                    const float bx = x0 + static_cast<float>(coord(poly[i + 2])), by = static_cast<float>(coord(poly[i + 3]));
                    out.push_back({origin + right * (ax * unit) + up * (ay * unit), origin + right * (bx * unit) + up * (by * unit), stroke,
                                   1.0f, colour, alpha});
                }
            }
        }
        x0 += kAdvance;
    }
}

} // namespace glyder::render
