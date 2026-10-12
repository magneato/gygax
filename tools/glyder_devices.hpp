#pragma once

// `gygax serve --glyder`: the machines registered with this node (robots, vehicles) shown in VR,
// and drivable from a headset when device commands are enabled.

#include <atomic>
#include <string>

#include <gygax/service/service.hpp>

namespace gygax::cli {

struct GlyderDevicesOptions {
    std::string code;      // pairing code (empty: a new one)
    bool open{false};      // no pairing (a private network only)
    std::uint16_t port{0}; // 0: glyder's default
    double originLat{0}, originLon{0};
    bool haveOrigin{false};
};

// Runs until `stop` is set. Reads devices through the service's own JSON-RPC, so every command a
// pilot sends goes through the same gate as the REST API (`--device-commands`).
int runGlyderDevices(service::Service& svc, const GlyderDevicesOptions& options, const std::atomic<bool>& stop);

} // namespace gygax::cli
