#include <gygax/service/logistics_api.hpp>

#include <algorithm>
#include <charconv>

#include <gygax/core/charconv.hpp>
#include <chrono>

#include <gygax/net/http.hpp>

namespace gygax::service {

namespace {

constexpr std::int64_t kDefaultPageSize = 200;
constexpr std::int64_t kMaximumPageSize = 10000;
constexpr std::size_t kMaximumWaypoints = 64;
constexpr double kPercentScale = 100.0;
constexpr double kMaximumReserveFraction = 0.9;

using logistics::Ledger;
using logistics::Outcome;
using logistics::Point;

LogisticsResult error(int status, std::string_view code, std::string_view message, json::Value details = json::Value::object()) {
    LogisticsResult r;
    r.status = status;
    r.body = json::Value::object();
    r.body["error"]["code"] = std::string(code);
    r.body["error"]["message"] = std::string(message);
    r.body["error"]["status"] = status;
    if (!details.asObject().empty()) r.body["error"]["details"] = std::move(details);
    return r;
}

LogisticsResult fromOutcome(Outcome o) {
    if (!o.ok) return error(o.status, o.code, o.message, std::move(o.body));
    return {o.status, std::move(o.body)};
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t intParam(const json::Value& p, std::string_view key, std::int64_t fallback) {
    const auto* v = p.find(key);
    if (v == nullptr) return fallback;
    if (v->isInt()) return v->asInt();
    if (v->isString()) {
        std::int64_t out = 0;
        const auto& s = v->asString();
        if (gygax::fromChars(s.data(), s.data() + s.size(), out).ec == std::errc()) return out;
    }
    return fallback;
}

double numberParam(const json::Value& p, std::string_view key, double fallback, bool* present = nullptr) {
    const auto* v = p.find(key);
    if (present != nullptr) *present = false;
    if (v == nullptr) return fallback;
    if (v->isNumber()) {
        if (present != nullptr) *present = true;
        return v->asDouble();
    }
    if (v->isString()) {
        const auto& s = v->asString();
        double out = 0.0;
        if (gygax::fromChars(s.data(), s.data() + s.size(), out).ec == std::errc()) {
            if (present != nullptr) *present = true;
            return out;
        }
    }
    return fallback;
}

std::optional<Point> pointOf(const json::Value* v) {
    if (v == nullptr || !v->isObject()) return std::nullopt;
    const auto* lat = v->find("latitude");
    const auto* lon = v->find("longitude");
    if (lat == nullptr || lon == nullptr || !lat->isNumber() || !lon->isNumber()) return std::nullopt;
    const Point p{lat->asDouble(), lon->asDouble()};
    constexpr double kMinimumLatitudeDegrees = -90.0;
    constexpr double kMaximumLatitudeDegrees = 90.0;
    constexpr double kMinimumLongitudeDegrees = -180.0;
    constexpr double kMaximumLongitudeDegrees = 180.0;
    if (!logistics::withinRange(p.latitude, kMinimumLatitudeDegrees, kMaximumLatitudeDegrees) ||
        !logistics::withinRange(p.longitude, kMinimumLongitudeDegrees, kMaximumLongitudeDegrees))
        return std::nullopt;
    return p;
}

json::Value pointJson(const Point& p) {
    json::Value o = json::Value::object();
    o["latitude"] = p.latitude;
    o["longitude"] = p.longitude;
    return o;
}

LogisticsResult plan(Ledger& ledger, const robotics::DeviceRegistry& devices, const json::Value& p) {
    logistics::PlanRequest req;
    std::string sku = p.getString("sku");
    std::optional<Point> origin = pointOf(p.find("origin"));
    double fuel = 1.0;
    std::string unitGuid = p.getString("unit");
    std::string fuelSource = "default";
    if (!unitGuid.empty()) {
        auto unit = ledger.unit(unitGuid);
        if (!unit) return error(net::kHttpStatusNotFound, "not_found", "no such unit");
        if (unit->getString("status") != "active") return error(net::kHttpStatusConflict, "conflict", "unit is not active");
        sku = unit->getString("sku");
        fuel = unit->getDouble("fuel", 1.0);
        fuelSource = "ledger";
        if (unit->find("latitude") != nullptr && !origin) origin = Point{unit->getDouble("latitude"), unit->getDouble("longitude")};
        const auto deviceId = unit->getString("device");
        if (!deviceId.empty()) {
            if (auto device = devices.get(deviceId); device && device->connected()) {
                const auto state = device->state();
                if (const auto* battery = state.find("battery"); battery != nullptr && battery->find("remaining_percent") != nullptr) {
                    const double pct = battery->getDouble("remaining_percent", -1.0);
                    if (pct >= 0.0 && pct <= kPercentScale) {
                        fuel = pct / kPercentScale;
                        fuelSource = "device";
                    }
                }
                if (!p.contains("origin") && state.contains("latitude") && state.contains("longitude") &&
                    state.getInt("heartbeats", 1) > 0) {
                    const Point live{state.getDouble("latitude"), state.getDouble("longitude")};
                    if (live.latitude != 0.0 || live.longitude != 0.0) origin = live;
                }
            }
        }
    }
    if (const auto site = p.getString("origin_site"); !site.empty() && !p.contains("origin")) {
        for (const auto& s : ledger.sites()) {
            if (s.id == site) origin = s.position;
        }
        if (!origin) return error(net::kHttpStatusNotFound, "not_found", "no such origin_site");
    }
    if (!origin)
        return error(net::kHttpStatusBadRequest, "invalid_request",
                     "an origin (latitude/longitude), origin_site or a positioned unit is required");
    req.origin = *origin;

    if (!sku.empty()) {
        if (auto model = ledger.model(sku)) {
            req.maxRangeM = model->maxRangeM;
            req.cruiseMps = model->cruiseMps;
            req.power = model->power;
        }
    }
    bool present = false;
    if (const auto v = numberParam(p, "max_range_m", 0.0, &present); present) req.maxRangeM = v;
    if (const auto v = numberParam(p, "cruise_mps", 0.0, &present); present) req.cruiseMps = v;
    if (const auto v = numberParam(p, "fuel", 1.0, &present); present) {
        fuel = v;
        fuelSource = "request";
    }
    if (const auto v = numberParam(p, "fuel_percent", kPercentScale, &present); present) {
        fuel = v / kPercentScale;
        fuelSource = "request";
    }
    req.reserveFraction = numberParam(p, "reserve", 0.1);
    if (const auto power = p.getString("power"); !power.empty()) req.power = power;
    if (!(req.maxRangeM > 0.0))
        return error(net::kHttpStatusBadRequest, "invalid_request",
                     "max_range_m is required (or register a model for the sku with max_range_m)");
    if (!logistics::withinRange(fuel, 0.0, 1.0)) return error(net::kHttpStatusBadRequest, "invalid_request", "fuel must be within 0..1");
    if (!logistics::withinRange(req.reserveFraction, 0.0, kMaximumReserveFraction))
        return error(net::kHttpStatusBadRequest, "invalid_request", "reserve must be within 0..0.9");
    req.fuelFraction = fuel;

    const auto* wps = p.find("waypoints");
    if (wps != nullptr && wps->isArray()) {
        if (wps->size() > kMaximumWaypoints) return error(net::kHttpStatusBadRequest, "invalid_request", "at most 64 waypoints");
        for (const auto& w : wps->asArray()) {
            auto pt = pointOf(&w);
            if (!pt) return error(net::kHttpStatusBadRequest, "invalid_request", "each waypoint needs latitude and longitude within range");
            req.waypoints.push_back(*pt);
        }
    }
    if (const auto* rt = p.find("return_to"); rt != nullptr) {
        auto pt = pointOf(rt);
        if (!pt) return error(net::kHttpStatusBadRequest, "invalid_request", "invalid return_to");
        req.returnTo = *pt;
    } else if (p.getBool("return_home")) {
        req.returnTo = req.origin;
    }
    if (req.waypoints.empty() && !req.returnTo)
        return error(net::kHttpStatusBadRequest, "invalid_request", "at least one waypoint is required");

    auto result = logistics::planRoute(req, ledger.sites());
    json::Value route = json::Value::array();
    if (const auto* legs = result.find("legs")) {
        for (const auto& leg : legs->asArray()) {
            json::Value stop = json::Value::object();
            if (const auto* to = leg.find("to")) stop = *to;
            stop["type"] = leg.getString("type");
            if (leg.contains("site")) stop["site"] = leg.getString("site");
            route.push(std::move(stop));
        }
    }
    result["route"] = std::move(route);
    json::Value inputs = json::Value::object();
    inputs["origin"] = pointJson(req.origin);
    inputs["max_range_m"] = req.maxRangeM;
    inputs["fuel_fraction"] = fuel;
    inputs["fuel_source"] = fuelSource;
    inputs["reserve_fraction"] = req.reserveFraction;
    inputs["power"] = req.power;
    if (!sku.empty()) inputs["sku"] = sku;
    if (!unitGuid.empty()) inputs["unit"] = unitGuid;
    result["inputs"] = std::move(inputs);
    return {net::kHttpStatusOk, std::move(result)};
}

} // namespace

LogisticsResult handleLogistics(Ledger& ledger, const robotics::DeviceRegistry& devices, std::string_view op, const json::Value& p) {
    const auto limit = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "limit", kDefaultPageSize), 1, kMaximumPageSize));
    if (op == "models.put") return fromOutcome(ledger.putModel(p));
    if (op == "models.list") {
        json::Value out = json::Value::object();
        out["models"] = ledger.models();
        return {net::kHttpStatusOk, std::move(out)};
    }
    if (op == "sites.put") return fromOutcome(ledger.putSite(p));
    if (op == "sites.remove") return fromOutcome(ledger.removeSite(p.getString("site")));
    if (op == "sites.list") {
        json::Value out = json::Value::object();
        out["sites"] = ledger.sitesJson();
        return {net::kHttpStatusOk, std::move(out)};
    }
    if (op == "events.add") return fromOutcome(ledger.record(p));
    if (op == "events.list") {
        const auto since = logistics::parseSince(p.contains("since") ? *p.find("since") : json::Value(), nowMs());
        if (!since)
            return error(net::kHttpStatusBadRequest, "invalid_request",
                         "since must be epoch seconds, epoch milliseconds, a duration like 7d, or an ISO date");
        json::Value out = json::Value::object();
        out["events"] = ledger.events(p.getString("sku"), *since, limit);
        return {net::kHttpStatusOk, std::move(out)};
    }
    if (op == "units.list") {
        json::Value out = json::Value::object();
        out["units"] = ledger.units(p.getString("sku"), p.getString("status"), limit);
        return {net::kHttpStatusOk, std::move(out)};
    }
    if (op == "units.get") {
        auto unit = ledger.unit(p.getString("guid"));
        if (!unit) return error(net::kHttpStatusNotFound, "not_found", "no such unit");
        return {net::kHttpStatusOk, std::move(*unit)};
    }
    if (op == "units.update") return fromOutcome(ledger.updateUnit(p.getString("guid"), p));
    if (op == "summary") {
        const auto since = logistics::parseSince(p.contains("since") ? *p.find("since") : json::Value("7d"), nowMs());
        if (!since)
            return error(400, "invalid_request", "since must be epoch seconds, epoch milliseconds, a duration like 7d, or an ISO date");
        return {200, ledger.summary(*since)};
    }
    if (op == "plan") return plan(ledger, devices, p);
    return error(404, "not_found", "unknown logistics operation");
}

}
