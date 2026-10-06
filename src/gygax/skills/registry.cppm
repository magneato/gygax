module;
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <gygax/core/errors.hpp>
#include <gygax/core/singleton.hpp>

export module gygax.tools;

export namespace gygax::tools {

using ToolFunction = std::function<std::string(const std::string&)>;

struct ToolInfo {
    std::string name;
    std::string description;
};

class ToolRegistry : public Singleton<ToolRegistry> {
    friend class Singleton<ToolRegistry>;

public:
    void registerTool(const std::string& name, ToolFunction tool, std::string description = {}) {
        std::lock_guard lock(mutex_);
        registry_[name] = Entry{std::move(tool), std::move(description)};
    }

    bool unregisterTool(const std::string& name) {
        std::lock_guard lock(mutex_);
        return registry_.erase(name) > 0;
    }

    [[nodiscard]] bool has(const std::string& name) const {
        std::lock_guard lock(mutex_);
        return registry_.contains(name);
    }

    [[nodiscard]] std::vector<ToolInfo> list() const {
        std::vector<ToolInfo> out;
        {
            std::lock_guard lock(mutex_);
            out.reserve(registry_.size());
            for (const auto& [name, entry] : registry_) out.push_back(ToolInfo{name, entry.description});
        }
        std::ranges::sort(out, {}, &ToolInfo::name);
        return out;
    }

    std::optional<std::string> execute(const std::string& name, const std::string& input) {
        ToolFunction fn;
        {
            std::lock_guard lock(mutex_);
            auto it = registry_.find(name);
            if (it == registry_.end()) return std::nullopt;
            fn = it->second.fn;
        }
        try {
            return fn(input);
        } catch (const std::exception& e) {
            throw ToolException(name, e.what());
        }
    }

private:
    struct Entry {
        ToolFunction fn;
        std::string description;
    };

    ToolRegistry() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> registry_;
};

}
