#pragma once

// The Construct: a small world glyder carries with it, so a headset is useful with no Gygax host
// on the network. A rover, a quadcopter, a kite and a truck go about their routines in the white
// void; drop into any of them. It runs on the same Host, wire protocol and Pilot interface as a
// real simulation (over loopback), so it exercises exactly the path a real host does.

#include <memory>
#include <string>

#include <glyder/host.hpp>

namespace glyder {

// The Construct's built-in scenes. Calm: slow, predictable routines and nothing that flashes.
// Chaos: a swarm, missiles, tracers and fast drones, all at once.
enum class DemoScene { Calm, Chaos };

class DemoWorld {
public:
    DemoWorld();
    ~DemoWorld();
    // Start the world and its host on loopback; returns the port to connect to.
    bool start(std::string* error, DemoScene scene = DemoScene::Calm);
    // Switch scenes while running; connected headsets get the new world without reconnecting.
    void setScene(DemoScene scene);
    [[nodiscard]] DemoScene scene() const;
    void stop();
    [[nodiscard]] std::uint16_t port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace glyder
