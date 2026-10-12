#pragma once

// OHFLOCK in glyder: the battle published to VR headsets, and a pilot able to drop into any kite.

#include <glyder/host.hpp>

#include "sim.hpp"

namespace ohflock {

class FlockPilot final : public glyder::Pilot {
public:
    explicit FlockPilot(Sim& sim) : sim_(sim) {}
    std::optional<std::string> refuse(std::uint32_t id) override;
    std::optional<Wait> busy(std::uint32_t id) override;
    glyder::Handling take(std::uint32_t id) override;
    void drive(std::uint32_t id, const glyder::wire::Control& c, bool fresh, double dt) override;
    void give(std::uint32_t id) override;
    bool exists(std::uint32_t id) override;
    [[nodiscard]] bool piloting() const { return !motion_.empty(); }

private:
    Sim& sim_;
    std::map<std::uint32_t, glyder::Motion> motion_;
};

// The dunes of the Sahran basin, the same field the HTML replay draws (heights in metres above the
// mean ground the simulation flies over).
[[nodiscard]] double ground(double x, double y);
[[nodiscard]] glyder::wire::Heightfield terrain();

// Publish the battle as it stands: units, shots, satellites, lasers, thrown sound, and the
// narrative since `fromEvent` (returns the new event count).
std::size_t publish(glyder::Host& host, const Sim& sim, std::size_t fromEvent);

[[nodiscard]] glyder::HostOptions hostOptions(const std::string& code, bool open, std::uint16_t port);

} // namespace ohflock
