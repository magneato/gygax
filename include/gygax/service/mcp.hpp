#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gygax/service/plugins.hpp>

namespace gygax::service {

std::optional<ExternalModule> connectMcp(const std::string& spec, std::string* error);

struct McpServerTool {
    std::string name;
    std::string description;
};

struct McpServerHandler {
    std::function<std::vector<McpServerTool>()> list;
    std::function<std::string(const std::string& name, const std::string& jsonArguments)> call;
};

int serveMcp(int inFd, int outFd, const std::string& serverName, const std::string& version, const McpServerHandler& handler);

}
