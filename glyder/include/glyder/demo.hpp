#pragma once

// The Construct: a small world glyder carries with it, so a headset is useful with no Gygax host
// on the network. A rover, a quadcopter, a kite and a truck go about their routines in the white
// void; drop into any of them. It runs on the same Host, wire protocol and Pilot interface as a
// real simulation (over loopback), so it exercises exactly the path a real host does.

#include <memory>
#include <string>

#include <glyder/host.hpp>

namespace glyder {

class DemoWorld {
public:
    DemoWorld();
    ~DemoWorld();
    // Start the world and its host on loopback; returns the port to connect to.
    bool start(std::string* error);
    void stop();
    [[nodiscard]] std::uint16_t port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace glyder
