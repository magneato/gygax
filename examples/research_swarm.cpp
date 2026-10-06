#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <gygax/brain/taxonomy.hpp>
#include <gygax/collective/consensus.hpp>
#include <gygax/core/json.hpp>
#include <gygax/transport/streams.hpp>

import gygax.core.base;

using namespace gygax;
using namespace gygax::taxonomy;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << "\n";
    if (!ok) ++failures;
}

json::Value telemetry(Brain& brain) {
    return json::parse(brain.telemetry()).value();
}

} // namespace

int main() {
    std::cout << "Gygax collective: consensus over heterogeneous embodiments\n";

    auto car = CreateVehicleModule(5001);
    auto uav = CreateUAVModule(6001);
    auto ship = CreateVesselModule(7001);

    auto& collective = CollectiveBrain::getInstance();
    collective.resetPool();
    collective.submitHypothesis({5001, "PATH_A_CLEAR", 0.75F});
    collective.submitHypothesis({6001, "PATH_A_OBSTRUCTED_BY_WEATHER", 0.98F});
    collective.submitHypothesis({7001, "WAIT_FOR_TIDE", 0.40F});
    const std::string consensus = collective.deriveConsensus();
    std::cout << "\nconsensus: " << consensus << "\n";
    check(consensus == "PATH_A_OBSTRUCTED_BY_WEATHER", "the most confident hypothesis wins");

    if (consensus.find("OBSTRUCTED") != std::string::npos) {
        car->applyBraking(100.0F);
        uav->setAltitude(500.0F);
        ship->setPropulsion(0.0F);
    }
    for (int i = 0; i < 200; ++i) {
        car->advance(0.05);
        uav->advance(0.05);
        ship->advance(0.05);
    }

    const auto carState = telemetry(*car);
    const auto uavState = telemetry(*uav);
    std::cout << "\ncar: " << carState.dump() << "\nuav: " << uavState.dump() << "\n\n";
    check(carState.getDouble("brake_pct") == 100.0 && carState.getDouble("speed_mps") == 0.0, "the ground vehicle braked to a stop");
    check(uavState.getDouble("altitude_m") == 50.0, "the UAV climbed at its 5 m/s limit for 10 s");
    check(json::parse(telemetry(*ship).dump()).has_value(), "the vessel reports telemetry");

    collective.resetPool();
    std::cout << "\n" << (failures == 0 ? "RESEARCH SWARM OK" : "RESEARCH SWARM FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
