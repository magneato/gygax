#pragma once

#include <cstddef>
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
    // JSON Schema for the arguments, as JSON text. Empty means {"type":"object"}.
    std::string inputSchema{};
};

struct McpServerHandler {
    std::function<std::vector<McpServerTool>()> list;
    std::function<std::string(const std::string& name, const std::string& jsonArguments)> call;
};

// Serves MCP over newline-delimited JSON-RPC until inFd closes. tools/call requests run on
// a pool of `workers` threads (0 = one per spare core, each pinned next to the reader's core)
// so several calls from one client proceed in parallel; replies may arrive out of order.
int serveMcp(int inFd, int outFd, const std::string& serverName, const std::string& version, const McpServerHandler& handler,
             std::size_t workers = 0);

}
