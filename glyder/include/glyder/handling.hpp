#pragma once

// How a node responds to a pilot's sticks. The host and the client run exactly this code, so the
// client can show the result of the pilot's input at once (no round trip) and the host's
// authoritative state agrees with it to within the network's jitter.

#include <cstdint>

#include <glyder/math.hpp>

namespace glyder {

struct Handling {
    enum class Model : std::uint8_t { None, Air, Ground, Hover };
    Model model{Model::None};
    float minSpeed{0}, maxSpeed{10}; // m/s (Air holds at least minSpeed)
    float yawRate{1};                // rad/s at full stick
    float climbRate{3};              // m/s at full stick
    float accel{4};                  // m/s^2
};

// A node's kinematic state in the world frame (ENU).
struct Motion {
    Vec3 pos;
    float heading{0}; // radians clockwise from north
    float speed{0};   // m/s along the heading (Air, Ground) or forward (Hover)
    float lateral{0}; // m/s to the right (Hover)
    float climb{0};   // m/s up
    float yawRate{0}; // rad/s, for banking
    [[nodiscard]] Vec3 velocity() const;
    [[nodiscard]] Quat attitude(Handling::Model model) const;
};

// The sticks, normalised -1..1 (wire::Control::axes order): forward, right, up, yaw right,
// pitch up, throttle.
using Sticks = float[6];

// Advance one step. `groundZ` keeps ground models on the ground and everything else above it.
void step(Motion& m, const Handling& h, const float (&axes)[6], float dt, float groundZ);
// Bring a node to a safe hold (stale controls): Air orbits gently at its speed, Ground and Hover stop.
void hold(Motion& m, const Handling& h, float dt, float groundZ);

} // namespace glyder
