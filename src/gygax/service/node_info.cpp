#include <gygax/service/node_info.hpp>

#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace gygax::service {

namespace {

constexpr std::size_t kCommandOutputBufferSize = 512;
constexpr std::size_t kHostnameBufferSize = 256;
constexpr std::size_t kNvidiaSmiColumnCount = 4;
constexpr std::chrono::seconds kGpuInfoCacheDuration{5};
constexpr std::int64_t kKiBPerMiB = 1024;

std::string trimCopy(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

std::string runCapture(const char* command) {
    std::string out;
    FILE* pipe = ::popen(command, "r");
    if (pipe == nullptr) return out;
    std::array<char, kCommandOutputBufferSize> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) out += buf.data();
    ::pclose(pipe);
    return out;
}

std::vector<GpuInfo> cachedGpus() {
    static std::mutex mutex;
    static std::vector<GpuInfo> cache;
    static std::chrono::steady_clock::time_point stamp{};
    std::lock_guard lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (stamp != std::chrono::steady_clock::time_point{} && now - stamp < kGpuInfoCacheDuration) return cache;
    stamp = now;
    cache.clear();
    if (::access("/usr/bin/nvidia-smi", X_OK) == 0 || ::access("/usr/local/bin/nvidia-smi", X_OK) == 0 ||
        ::access("/bin/nvidia-smi", X_OK) == 0) {
        cache = parseNvidiaSmi(runCapture(
            "timeout 2 nvidia-smi --query-gpu=name,memory.total,memory.used,utilization.gpu --format=csv,noheader,nounits 2>/dev/null"));
    }
    return cache;
}

}

std::vector<GpuInfo> parseNvidiaSmi(const std::string& csv) {
    std::vector<GpuInfo> gpus;
    std::istringstream lines(csv);
    std::string line;
    while (std::getline(lines, line)) {
        std::vector<std::string> fields;
        std::istringstream row(line);
        std::string field;
        while (std::getline(row, field, ',')) fields.push_back(trimCopy(field));
        if (fields.size() < kNvidiaSmiColumnCount) continue;
        GpuInfo gpu;
        gpu.name = fields[0];
        try {
            gpu.memoryTotalMb = std::stoll(fields[1]);
            gpu.memoryUsedMb = std::stoll(fields[2]);
            gpu.utilizationPct = std::stod(fields[3]);
        } catch (const std::exception&) {
            continue;
        }
        gpus.push_back(std::move(gpu));
    }
    return gpus;
}

NodeInfo collectNodeInfo(bool includeGpu) {
    NodeInfo info;
    char host[kHostnameBufferSize] = {};
    if (::gethostname(host, sizeof(host) - 1) == 0) info.hostname = host;
    utsname un{};
    if (::uname(&un) == 0) {
        info.os = std::string(un.sysname) + " " + un.release;
        info.arch = un.machine;
    }
    info.cpuCores = std::max(1U, std::thread::hardware_concurrency());

    if (std::ifstream cpu("/proc/cpuinfo"); cpu) {
        std::string line;
        while (std::getline(cpu, line)) {
            if (line.starts_with("model name")) {
                const auto colon = line.find(':');
                if (colon != std::string::npos) info.cpuModel = trimCopy(line.substr(colon + 1));
                break;
            }
        }
    }
    if (std::ifstream mem("/proc/meminfo"); mem) {
        std::string key;
        std::int64_t value = 0;
        std::string line;
        while (std::getline(mem, line)) {
            std::istringstream row(line);
            if (!(row >> key >> value)) continue;
            if (key == "MemTotal:") info.memoryTotalMb = value / kKiBPerMiB;
            if (key == "MemAvailable:") info.memoryAvailableMb = value / kKiBPerMiB;
        }
    }
    if (std::ifstream load("/proc/loadavg"); load) load >> info.load1 >> info.load5 >> info.load15;
    if (std::ifstream up("/proc/uptime"); up) {
        double seconds = 0;
        up >> seconds;
        info.uptimeSeconds = static_cast<std::int64_t>(seconds);
    }
    if (includeGpu) info.gpus = cachedGpus();
    return info;
}

json::Value toJson(const NodeInfo& info) {
    json::Value out = json::Value::object();
    out["hostname"] = info.hostname;
    out["os"] = info.os;
    out["arch"] = info.arch;
    out["cpu_model"] = info.cpuModel;
    out["cpu_cores"] = info.cpuCores;
    out["memory_total_mb"] = info.memoryTotalMb;
    out["memory_available_mb"] = info.memoryAvailableMb;
    out["load"] = json::Value::array();
    out["load"].push(info.load1);
    out["load"].push(info.load5);
    out["load"].push(info.load15);
    out["uptime_seconds"] = info.uptimeSeconds;
    json::Value gpus = json::Value::array();
    for (const auto& g : info.gpus) {
        json::Value item = json::Value::object();
        item["name"] = g.name;
        item["memory_total_mb"] = g.memoryTotalMb;
        item["memory_used_mb"] = g.memoryUsedMb;
        item["utilization_pct"] = g.utilizationPct;
        gpus.push(std::move(item));
    }
    out["gpus"] = std::move(gpus);
    return out;
}

}
