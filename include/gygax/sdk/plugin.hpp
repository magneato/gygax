#pragma once

#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <vector>

#include <gygax/sdk/plugin.h>

namespace gygax::sdk {

/** Receives UTF-8 tool input and returns UTF-8 output or throws an error. */
using ToolFunction = std::function<std::string(const std::string& input)>;

/**
 * Small C++ adapter for the versioned C plugin ABI.
 *
 * Construct and register tools before exporting the descriptor. Keep this
 * object alive until the host unloads the plugin. Tool functions can be called
 * concurrently and must be safe for concurrent use. Native plugins run inside
 * the host process with its privileges; prefer an isolated extension process
 * when code is not fully trusted.
 */
class Plugin {
public:
    Plugin(std::string name, std::string version) : name_(std::move(name)), version_(std::move(version)) {}

    Plugin& tool(std::string name, std::string description, ToolFunction fn) {
        entries_.push_back({std::move(name), std::move(description), std::move(fn)});
        return *this;
    }

    /** Build and return the descriptor; call during plugin startup. */
    const gygax_plugin* describe() {
        exported_.clear();
        for (const auto& e : entries_) exported_.push_back({e.name.c_str(), e.description.c_str()});
        api_ = gygax_plugin{GYGAX_PLUGIN_ABI_VERSION, name_.c_str(),       version_.c_str(), exported_.size(),
                            exported_.data(),         &Plugin::trampoline, &Plugin::release, &Plugin::shutdown};
        current() = this;
        return &api_;
    }

private:
    struct Entry {
        std::string name;
        std::string description;
        ToolFunction fn;
    };

    static Plugin*& current() {
        static Plugin* instance = nullptr;
        return instance;
    }

    static char* duplicate(const std::string& text) {
        char* out = static_cast<char*>(std::malloc(text.size() + 1));
        if (out != nullptr) std::memcpy(out, text.c_str(), text.size() + 1);
        return out;
    }

    static int trampoline(const char* tool, const char* input, char** output) {
        *output = nullptr;
        Plugin* self = current();
        if (self == nullptr || tool == nullptr) return 1;
        for (const auto& e : self->entries_) {
            if (e.name != tool) continue;
            try {
                *output = duplicate(e.fn(input == nullptr ? std::string() : std::string(input)));
                return *output == nullptr ? 1 : 0;
            } catch (const std::exception& ex) {
                *output = duplicate(ex.what());
                return 1;
            } catch (...) {
                *output = duplicate("unknown error");
                return 1;
            }
        }
        *output = duplicate("unknown tool");
        return 1;
    }

    static void release(char* buffer) { std::free(buffer); }
    static void shutdown() {}

    std::string name_;
    std::string version_;
    std::vector<Entry> entries_;
    std::vector<gygax_plugin_tool> exported_;
    gygax_plugin api_{};
};

}

#define GYGAX_PLUGIN(instance)                                                                                                             \
    extern "C" GYGAX_PLUGIN_EXPORT const gygax_plugin* gygax_plugin_entry(void) {                                                          \
        return (instance).describe();                                                                                                      \
    }
