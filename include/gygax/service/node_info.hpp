#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <gygax/core/json.hpp>

namespace gygax::service {

struct GpuInfo {
    std::string name;
    std::int64_t memoryTotalMb = 0;
    std::int64_t memoryUsedMb = 0;
    double utilizationPct = 0.0;
};

struct NodeInfo {
    std::string hostname;
    std::string os;
    std::string arch;
    std::string cpuModel;
    unsigned cpuCores = 0;
    std::int64_t memoryTotalMb = 0;
    std::int64_t memoryAvailableMb = 0;
    double load1 = 0.0;
    double load5 = 0.0;
    double load15 = 0.0;
    std::int64_t uptimeSeconds = 0;
    std::vector<GpuInfo> gpus;
};

NodeInfo collectNodeInfo(bool includeGpu = true);
json::Value toJson(const NodeInfo& info);
std::vector<GpuInfo> parseNvidiaSmi(const std::string& csv);

}
