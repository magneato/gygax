#include <glyder/handling.hpp>

#include <algorithm>
#include <cmath>

namespace glyder {

Vec3 Motion::velocity() const {
    const float s = std::sin(heading), c = std::cos(heading);
    // Forward is (sin h, cos h) in ENU; right is (cos h, -sin h).
    return {s * speed + c * lateral, c * speed - s * lateral, climb};
}

Quat Motion::attitude(Handling::Model model) const {
    if (model == Handling::Model::Ground) return glyder::attitude(heading);
    const float pitch = std::atan2(climb, std::max(std::fabs(speed), 1.0f)) * (model == Handling::Model::Air ? 1.0f : 0.3f);
    // Coordinated turn: bank = atan(v * yaw rate / g), capped.
    float bank = std::atan(speed * yawRate / 9.81f);
    if (model == Handling::Model::Hover) bank = clamp(lateral * 0.08f, -0.35f, 0.35f) + clamp(yawRate * 0.05f, -0.1f, 0.1f);
    return glyder::attitude(heading, pitch, clamp(bank, -1.1f, 1.1f));
}

namespace {
float approach(float v, float target, float rate, float dt) {
    const float d = target - v, step = rate * dt;
    return std::fabs(d) <= step ? target : v + std::copysign(step, d);
}
float wrap(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}
} // namespace

void step(Motion& m, const Handling& h, const float (&axes)[6], float dt, float groundZ) {
    const float fwd = clamp(axes[0], -1, 1), right = clamp(axes[1], -1, 1), up = clamp(axes[2], -1, 1), yaw = clamp(axes[3], -1, 1),
                pitch = clamp(axes[4], -1, 1);
    switch (h.model) {
    case Handling::Model::Air: {
        // Forward stick trims speed, yaw turns, pitch (or up) climbs.
        const float target = clamp(m.speed + fwd * h.accel * 1.5f, h.minSpeed, h.maxSpeed);
        m.speed = approach(m.speed, target, h.accel, dt);
        m.yawRate = approach(m.yawRate, yaw * h.yawRate, h.yawRate * 3.0f, dt);
        m.climb = approach(m.climb, clamp(pitch + up, -1, 1) * h.climbRate, h.climbRate * 2.0f, dt);
        m.lateral = 0;
        break;
    }
    case Handling::Model::Ground: {
        const float target = fwd >= 0 ? fwd * h.maxSpeed : fwd * h.maxSpeed * 0.4f;
        m.speed = approach(m.speed, target, h.accel, dt);
        const float turn = std::fabs(m.speed) < 0.2f ? 0.0f : (m.speed >= 0 ? 1.0f : -1.0f);
        m.yawRate = approach(m.yawRate, yaw * h.yawRate * turn, h.yawRate * 4.0f, dt);
        m.climb = 0;
        m.lateral = 0;
        break;
    }
    case Handling::Model::Hover: {
        m.speed = approach(m.speed, fwd * h.maxSpeed, h.accel, dt);
        m.lateral = approach(m.lateral, right * h.maxSpeed, h.accel, dt);
        m.climb = approach(m.climb, clamp(up + pitch, -1, 1) * h.climbRate, h.climbRate * 2.0f, dt);
        m.yawRate = approach(m.yawRate, yaw * h.yawRate, h.yawRate * 4.0f, dt);
        break;
    }
    case Handling::Model::None: return;
    }
    m.heading = wrap(m.heading + m.yawRate * dt);
    m.pos += m.velocity() * dt;
    if (h.model == Handling::Model::Ground) {
        m.pos.z = groundZ;
    } else if (m.pos.z < groundZ + 2.0f && h.model == Handling::Model::Air) {
        m.pos.z = groundZ + 2.0f; // a terrain floor: an aircraft cannot be flown into the ground
        m.climb = std::max(m.climb, 0.0f);
    } else if (m.pos.z < groundZ) {
        m.pos.z = groundZ;
        m.climb = std::max(m.climb, 0.0f);
    }
}

void hold(Motion& m, const Handling& h, float dt, float groundZ) {
    const float none[6] = {0, 0, 0, 0, 0, 0};
    if (h.model == Handling::Model::Air) {
        // A gentle standard-rate orbit at the current speed and height.
        const float orbit[6] = {0, 0, 0, 0.25f, 0, 0};
        m.climb = 0;
        step(m, h, orbit, dt, groundZ);
        return;
    }
    step(m, h, none, dt, groundZ);
}

} // namespace glyder
