#pragma once

// Colour vision: what a viewer with a missing cone type sees, and a correction that moves the
// colour differences they would lose into ones they can still see.
//
//   Protan  no long-wavelength (red) cones
//   Deutan  no medium-wavelength (green) cones
//   Tritan  no short-wavelength (blue) cones
//
// Simulation uses the Machado, Oliveira and Fernandes (2009) matrices at full severity, in linear
// RGB. Correction is daltonization (Fidaner, Lin and Ozguven 2005): the error between the scene and
// its simulation is redistributed into channels that viewer perceives. Both are 3 x 3 matrices, so
// the renderer applies them in its final pass to everything on screen, world and interface alike.

#include <array>
#include <string_view>

#include <glyder/math.hpp>

namespace glyder::colour {

enum class Vision : std::uint8_t { Typical, Protan, Deutan, Tritan };

[[nodiscard]] std::string_view name(Vision v);  // "NO BLUE (TRITAN)", for the interface
[[nodiscard]] std::string_view token(Vision v); // "tritan", for settings and command lines
[[nodiscard]] Vision fromToken(std::string_view t, Vision fallback = Vision::Typical);
// The order a menu cycles through. Tritan comes first after typical.
[[nodiscard]] Vision next(Vision v);

// Row-major 3 x 3, acting on linear RGB column vectors.
struct Mat3 {
    std::array<float, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};
    [[nodiscard]] Vec3 apply(Vec3 c) const {
        return {m[0] * c.x + m[1] * c.y + m[2] * c.z, m[3] * c.x + m[4] * c.y + m[5] * c.z, m[6] * c.x + m[7] * c.y + m[8] * c.z};
    }
};
[[nodiscard]] Mat3 operator*(const Mat3& a, const Mat3& b);

// What a viewer with this vision sees (identity for Typical).
[[nodiscard]] Mat3 simulate(Vision v);
// The correction for this viewer (identity for Typical).
[[nodiscard]] Mat3 correct(Vision v);

// The matrix the final pass applies: the correction when `corrected`, then, when `preview`, the
// simulation, so a viewer with typical colour vision sees what this viewer would.
[[nodiscard]] Mat3 display(Vision v, bool corrected, bool preview);

} // namespace glyder::colour
