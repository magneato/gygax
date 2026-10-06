#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gygax::service {

struct ExternalTool {
    std::string name;
    std::string description;
    std::function<std::string(const std::string&)> invoke;
};

struct ExternalModule {
    std::string kind;
    std::string name;
    std::string version;
    std::string source;
    std::vector<ExternalTool> tools;
    std::shared_ptr<void> keepAlive;
};

std::optional<ExternalModule> loadPlugin(const std::string& path, std::string* error);
std::optional<ExternalModule> connectExtension(const std::string& spec, std::string* error);
bool validPluginName(const std::string& name);
bool validPluginToolName(const std::string& name);

}
