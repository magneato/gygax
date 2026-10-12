#pragma once

// WarGames in glyder: the last stand in VR, with a pilot able to take any mobile defender.

#include <map>
#include <memory>
#include <set>
#include <string>

#include <glyder/handling.hpp>
#include <glyder/host.hpp>

#include "battlefield.hpp"

namespace wargames {

class WarVr final : public glyder::Pilot {
public:
    WarVr(Battlefield& field, std::string code, bool open, std::uint16_t port);
    bool start(std::string* error);
    void stop();
    // Serve the headsets: drop-ins, a pilot's sticks, and the world as it stands. Call often.
    void tick(double realSeconds);
    void event(const std::string& line);
    [[nodiscard]] bool manual(std::uint32_t id) const { return motion_.count(id) > 0; }
    [[nodiscard]] std::vector<std::string> takeOrders() { return host_.takeOrders(); }
    [[nodiscard]] std::string banner() const;

    // Pilot
    std::optional<std::string> refuse(std::uint32_t id) override;
    std::optional<Wait> busy(std::uint32_t id) override;
    glyder::Handling take(std::uint32_t id) override;
    void drive(std::uint32_t id, const glyder::wire::Control& c, bool fresh, double dt) override;
    void give(std::uint32_t id) override;
    bool exists(std::uint32_t id) override;

private:
    void publish();
    Battlefield& field_;
    glyder::Host host_;
    std::map<std::uint32_t, glyder::Motion> motion_;
    std::map<std::uint32_t, std::pair<double, Vec>> last_; // for velocities between ticks
    std::map<std::uint32_t, glyder::Vec3> velocity_;
    double clock_{0};
};

} // namespace wargames
