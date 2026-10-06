#include <cmath>
#include <stdexcept>
#include <string>

#include <gygax/core/json.hpp>
#include <gygax/sdk/plugin.hpp>

namespace {

gygax::sdk::Plugin& plugin() {
    static gygax::sdk::Plugin p = [] {
        gygax::sdk::Plugin built("fleet", "0.1.0");
        built.tool("sorties_needed", "Sorties needed to move {items} when each sortie carries {capacity}. Input JSON.",
                   [](const std::string& input) {
                       auto doc = gygax::json::parse(input);
                       if (!doc || !doc->isObject()) throw std::invalid_argument("expected a JSON object");
                       const double items = doc->getDouble("items");
                       const double capacity = doc->getDouble("capacity");
                       if (!(capacity > 0.0) || items < 0.0)
                           throw std::invalid_argument("capacity must be positive and items non-negative");
                       return std::to_string(static_cast<long long>(std::ceil(items / capacity)));
                   });
        return built;
    }();
    return p;
}

}

GYGAX_PLUGIN(plugin())
