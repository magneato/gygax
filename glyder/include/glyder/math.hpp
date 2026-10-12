#pragma once

// Small, exact, allocation-free 3D math for glyder: float vectors, quaternions and column-major 4x4
// matrices with Vulkan clip conventions (y down, depth 0..1, reversed-Z available).
//
// Frames used throughout glyder:
//   world  ENU metres, the simulations' frame: x east, y north, z up.
//   model  FLU metres, every kit mesh: x forward (nose), y left, z up.
//   render the OpenXR / Vulkan view frame: x right, y up, -z forward. World maps to render by
//          render = (east, up, -north), see toRender().

#include <cmath>
#include <cstdint>

namespace glyder {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kDeg = kPi / 180.0f;

struct Vec2 {
    float x{0}, y{0};
};

struct Vec3 {
    float x{0}, y{0}, z{0};
    constexpr Vec3& operator+=(Vec3 b) {
        x += b.x;
        y += b.y;
        z += b.z;
        return *this;
    }
    constexpr Vec3& operator-=(Vec3 b) {
        x -= b.x;
        y -= b.y;
        z -= b.z;
        return *this;
    }
    constexpr Vec3& operator*=(float s) {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }
};

struct Vec4 {
    float x{0}, y{0}, z{0}, w{0};
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] constexpr Vec3 operator-(Vec3 a) {
    return {-a.x, -a.y, -a.z};
}
[[nodiscard]] constexpr Vec3 operator*(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
[[nodiscard]] constexpr Vec3 operator*(float s, Vec3 a) {
    return a * s;
}
[[nodiscard]] constexpr Vec3 operator/(Vec3 a, float s) {
    return a * (1.0f / s);
}
[[nodiscard]] constexpr Vec3 mul(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}
[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline float length(Vec3 a) {
    return std::sqrt(dot(a, a));
}
[[nodiscard]] inline Vec3 normalize(Vec3 a) {
    const float l = length(a);
    return l > 1e-20f ? a / l : Vec3{0, 0, 0};
}
[[nodiscard]] constexpr Vec3 lerp(Vec3 a, Vec3 b, float t) {
    return a + (b - a) * t;
}
[[nodiscard]] constexpr float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}
[[nodiscard]] constexpr float clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
[[nodiscard]] constexpr float saturate(float v) {
    return clamp(v, 0.0f, 1.0f);
}
[[nodiscard]] constexpr float smoothstep(float e0, float e1, float x) {
    const float t = saturate((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

// World ENU to the render frame and back.
[[nodiscard]] constexpr Vec3 toRender(Vec3 enu) {
    return {enu.x, enu.z, -enu.y};
}
[[nodiscard]] constexpr Vec3 fromRender(Vec3 r) {
    return {r.x, -r.z, r.y};
}

struct Quat {
    float x{0}, y{0}, z{0}, w{1};
};

[[nodiscard]] constexpr Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
[[nodiscard]] constexpr Quat conjugate(Quat q) {
    return {-q.x, -q.y, -q.z, q.w};
}
[[nodiscard]] inline Quat normalize(Quat q) {
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-20f ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}
[[nodiscard]] inline Quat axisAngle(Vec3 axis, float radians) {
    const Vec3 a = normalize(axis);
    const float s = std::sin(radians * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
}
[[nodiscard]] constexpr Vec3 rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
[[nodiscard]] inline Quat slerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f) return normalize(Quat{lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)});
    const float th = std::acos(d), s = std::sin(th);
    const float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

// Attitude in the world ENU frame for a model authored FLU: heading clockwise from north (the compass
// convention every simulation uses), pitch nose-up positive, roll right-wing-down positive.
[[nodiscard]] inline Quat attitude(float headingRad, float pitchRad = 0, float rollRad = 0) {
    // FLU x (forward) must point east at heading 90 deg: yaw about +z by (90 deg - heading).
    const Quat yaw = axisAngle({0, 0, 1}, kPi * 0.5f - headingRad);
    const Quat pitch = axisAngle({0, 1, 0}, -pitchRad); // +y is left; nose up is a negative rotation about it
    const Quat roll = axisAngle({1, 0, 0}, rollRad);
    return yaw * pitch * roll;
}
// The attitude that points the model's nose along a world velocity, banked into the turn when
// lateral acceleration is given.
[[nodiscard]] inline Quat alongVelocity(Vec3 v, float bankRad = 0) {
    const float h = std::sqrt(v.x * v.x + v.y * v.y);
    if (h < 1e-4f && std::fabs(v.z) < 1e-4f) return attitude(0);
    return attitude(std::atan2(v.x, v.y), std::atan2(v.z, h), bankRad);
}

// Column-major 4x4: m[c][r].
struct Mat4 {
    float m[4][4]{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    [[nodiscard]] static constexpr Mat4 identity() { return {}; }
    [[nodiscard]] constexpr Vec3 transformPoint(Vec3 p) const {
        return {m[0][0] * p.x + m[1][0] * p.y + m[2][0] * p.z + m[3][0], m[0][1] * p.x + m[1][1] * p.y + m[2][1] * p.z + m[3][1],
                m[0][2] * p.x + m[1][2] * p.y + m[2][2] * p.z + m[3][2]};
    }
    [[nodiscard]] constexpr Vec3 transformDir(Vec3 d) const {
        return {m[0][0] * d.x + m[1][0] * d.y + m[2][0] * d.z, m[0][1] * d.x + m[1][1] * d.y + m[2][1] * d.z,
                m[0][2] * d.x + m[1][2] * d.y + m[2][2] * d.z};
    }
    [[nodiscard]] constexpr Vec4 operator*(Vec4 v) const {
        return {
            m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z + m[3][0] * v.w, m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z + m[3][1] * v.w,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z + m[3][2] * v.w, m[0][3] * v.x + m[1][3] * v.y + m[2][3] * v.z + m[3][3] * v.w};
    }
};

[[nodiscard]] constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k][row] * b.m[c][k];
            r.m[c][row] = s;
        }
    return r;
}

[[nodiscard]] constexpr Mat4 translation(Vec3 t) {
    Mat4 r;
    r.m[3][0] = t.x;
    r.m[3][1] = t.y;
    r.m[3][2] = t.z;
    return r;
}
[[nodiscard]] constexpr Mat4 scaling(Vec3 s) {
    Mat4 r;
    r.m[0][0] = s.x;
    r.m[1][1] = s.y;
    r.m[2][2] = s.z;
    return r;
}
[[nodiscard]] constexpr Mat4 rotation(Quat q) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 r;
    r.m[0][0] = 1 - 2 * (yy + zz);
    r.m[0][1] = 2 * (xy + wz);
    r.m[0][2] = 2 * (xz - wy);
    r.m[1][0] = 2 * (xy - wz);
    r.m[1][1] = 1 - 2 * (xx + zz);
    r.m[1][2] = 2 * (yz + wx);
    r.m[2][0] = 2 * (xz + wy);
    r.m[2][1] = 2 * (yz - wx);
    r.m[2][2] = 1 - 2 * (xx + yy);
    return r;
}
[[nodiscard]] constexpr Mat4 compose(Vec3 t, Quat q, Vec3 s = {1, 1, 1}) {
    return translation(t) * rotation(q) * scaling(s);
}

// The world (ENU) to render (Y-up) change of basis as a matrix.
[[nodiscard]] constexpr Mat4 worldToRender() {
    Mat4 r;
    r.m[1][1] = 0;
    r.m[2][2] = 0;
    r.m[1][2] = -1; // north -> -z
    r.m[2][1] = 1;  // up -> y
    return r;       // east -> x stays from the identity
}

// Inverse of a rigid transform (rotation + translation).
[[nodiscard]] constexpr Mat4 rigidInverse(const Mat4& a) {
    Mat4 r;
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row) r.m[c][row] = a.m[row][c];
    const Vec3 t{a.m[3][0], a.m[3][1], a.m[3][2]};
    const Vec3 it = r.transformDir(t);
    r.m[3][0] = -it.x;
    r.m[3][1] = -it.y;
    r.m[3][2] = -it.z;
    return r;
}

// General inverse (cofactor expansion); returns identity for a singular matrix.
[[nodiscard]] Mat4 inverse(const Mat4& a);

// View matrix looking from `eye` at `target` with `up` roughly up (render frame).
[[nodiscard]] inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = normalize(target - eye);
    if (length(f) == 0.0f) f = {0, 0, -1};
    Vec3 s = normalize(cross(f, up));
    if (length(s) == 0.0f) s = {1, 0, 0};
    const Vec3 u = cross(s, f);
    Mat4 m;
    m.m[0][0] = s.x, m.m[1][0] = s.y, m.m[2][0] = s.z;
    m.m[0][1] = u.x, m.m[1][1] = u.y, m.m[2][1] = u.z;
    m.m[0][2] = -f.x, m.m[1][2] = -f.y, m.m[2][2] = -f.z;
    m.m[3][0] = -dot(s, eye), m.m[3][1] = -dot(u, eye), m.m[3][2] = dot(f, eye);
    return m;
}

// View matrix of an eye at `position` with orientation `q` (render frame).
[[nodiscard]] inline Mat4 viewFromPose(Vec3 position, Quat q) {
    return rigidInverse(compose(position, q));
}

// Asymmetric-frustum projection from OpenXR field-of-view tangents, Vulkan clip space, reversed-Z
// with an infinite far plane: depth 1 at `nearZ`, tending to 0 at infinity. Reversed-Z spends float
// precision where it is needed, so a 20 km battle and a cockpit 30 cm away share one depth buffer.
[[nodiscard]] inline Mat4 projection(float tanLeft, float tanRight, float tanUp, float tanDown, float nearZ) {
    const float w = tanRight - tanLeft, h = tanUp - tanDown; // tanDown negative, tanUp positive
    Mat4 p{};
    p.m[0][0] = 2.0f / w;
    p.m[1][1] = -2.0f / h; // Vulkan: y down
    p.m[2][0] = (tanRight + tanLeft) / w;
    p.m[2][1] = -(tanUp + tanDown) / h;
    p.m[2][2] = 0.0f;
    p.m[2][3] = -1.0f;
    p.m[3][2] = nearZ;
    p.m[3][3] = 0.0f;
    return p;
}
[[nodiscard]] inline Mat4 perspective(float verticalFovRad, float aspect, float nearZ) {
    const float t = std::tan(verticalFovRad * 0.5f);
    return projection(-t * aspect, t * aspect, t, -t, nearZ);
}

// sRGB byte colour.
struct Rgb8 {
    std::uint8_t r{0}, g{0}, b{0};
};
[[nodiscard]] constexpr Rgb8 hex(std::uint32_t c) {
    return {static_cast<std::uint8_t>(c >> 16), static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c)};
}

} // namespace glyder
