#pragma once

#include <string_view>

#include <gygax/core/json.hpp>
#include <gygax/logistics/ledger.hpp>
#include <gygax/robotics/device.hpp>

namespace gygax::service {

struct LogisticsResult {
    int status = 200;
    json::Value body = json::Value::object();
};

LogisticsResult handleLogistics(logistics::Ledger& ledger, const robotics::DeviceRegistry& devices, std::string_view op,
                                const json::Value& params);

}
