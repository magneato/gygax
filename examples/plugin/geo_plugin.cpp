#include <cmath>
#include <format>
#include <stdexcept>

#include <gygax/core/json.hpp>
#include <gygax/sdk/plugin.hpp>

namespace {

constexpr double kEarthMeanRadiusMeters = 6371008.8;
constexpr double kDegreesPerHalfTurn = 180.0;

double haversine(double lat1, double lon1, double lat2, double lon2) {
    constexpr double pi = 3.14159265358979323846;
    const auto rad = [](double d) { return d * pi / kDegreesPerHalfTurn; };
    const double a = std::pow(std::sin(rad(lat2 - lat1) / 2), 2) +
                     std::cos(rad(lat1)) * std::cos(rad(lat2)) * std::pow(std::sin(rad(lon2 - lon1) / 2), 2);
    return 2.0 * kEarthMeanRadiusMeters * std::asin(std::sqrt(a));
}

gygax::sdk::Plugin& plugin() {
    static gygax::sdk::Plugin p = [] {
        gygax::sdk::Plugin built("geo", "1.0.0");
        built.tool("distance", "Great-circle distance in meters. Input: {\"from\":{latitude,longitude},\"to\":{latitude,longitude}}",
                   [](const std::string& input) {
                       auto doc = gygax::json::parse(input);
                       if (!doc || !doc->isObject() || doc->find("from") == nullptr || doc->find("to") == nullptr)
                           throw std::invalid_argument("expected {\"from\":{...},\"to\":{...}}");
                       const auto& from = *doc->find("from");
                       const auto& to = *doc->find("to");
                       return std::format("{:.1f}", haversine(from.getDouble("latitude"), from.getDouble("longitude"),
                                                              to.getDouble("latitude"), to.getDouble("longitude")));
                   });
        built.tool("echo", "Return the input unchanged", [](const std::string& input) { return input; });
        return built;
    }();
    return p;
}

}

GYGAX_PLUGIN(plugin())
