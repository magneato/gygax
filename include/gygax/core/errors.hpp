#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace gygax {

class GygaxException : public std::runtime_error {
public:
    explicit GygaxException(const std::string& message) : std::runtime_error(message) {}
};

class AgentNotFoundException : public GygaxException {
public:
    explicit AgentNotFoundException(uint32_t sid) : GygaxException("Agent with SID " + std::to_string(sid) + " not found") {}
};

class LinkException : public GygaxException {
public:
    explicit LinkException(const std::string& linkName) : GygaxException("Link error: " + linkName) {}
};

class ToolException : public GygaxException {
public:
    ToolException(const std::string& toolName, const std::string& detail) : GygaxException("Tool '" + toolName + "' error: " + detail) {}
};

}
