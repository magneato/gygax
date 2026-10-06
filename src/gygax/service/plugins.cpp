#include <gygax/service/plugins.hpp>

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include <gygax/core/json.hpp>
#include <gygax/net/http.hpp>
#include <gygax/sdk/plugin.h>

namespace gygax::service {

namespace {

constexpr std::size_t kMaxOutput = 4 * 1024 * 1024;
constexpr std::size_t kMaxPluginNameLength = 32;
constexpr std::size_t kMaxPluginToolNameLength = 48;
constexpr std::size_t kMaxPluginTools = 128;
constexpr std::chrono::seconds kExtensionReadTimeout{10};

bool allowedChars(const std::string& s, std::size_t max, bool lowerOnly) {
    if (s.empty() || s.size() > max) return false;
    return std::all_of(s.begin(), s.end(), [&](char c) {
        const bool letter = lowerOnly ? (c >= 'a' && c <= 'z') : std::isalpha(static_cast<unsigned char>(c)) != 0;
        return letter || std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
    });
}

struct Library {
    void* handle = nullptr;
    const gygax_plugin* api = nullptr;

    ~Library() {
        if (api != nullptr && api->shutdown != nullptr) api->shutdown();
        if (handle != nullptr) dlclose(handle);
    }
};

} // namespace

bool validPluginName(const std::string& name) {
    return allowedChars(name, kMaxPluginNameLength, true);
}
bool validPluginToolName(const std::string& name) {
    return allowedChars(name, kMaxPluginToolNameLength, false);
}

std::optional<ExternalModule> loadPlugin(const std::string& path, std::string* error) {
    auto fail = [&](std::string m) -> std::optional<ExternalModule> {
        if (error != nullptr) *error = "plugin " + path + ": " + std::move(m);
        return std::nullopt;
    };
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) == nullptr) return fail("file not found");
    struct stat st {};
    if (::stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) return fail("not a regular file");
    if ((st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & S_IWOTH) != 0 ||
        ((st.st_mode & S_IWGRP) != 0 && st.st_gid != getegid()))
        return fail(
            "refusing to load: the file must be owned by root or the service user and not writable by others or by a foreign group");
    auto lib = std::make_shared<Library>();
    lib->handle = dlopen(resolved, RTLD_NOW | RTLD_LOCAL);
    if (lib->handle == nullptr) return fail(dlerror());
    using Entry = const gygax_plugin* (*)();
    auto entry = reinterpret_cast<Entry>(dlsym(lib->handle, "gygax_plugin_entry"));
    if (entry == nullptr) return fail("missing gygax_plugin_entry");
    const auto* api = entry();
    if (api == nullptr) return fail("plugin returned no descriptor");
    if (api->abi_version != GYGAX_PLUGIN_ABI_VERSION)
        return fail("ABI version " + std::to_string(api->abi_version) + " does not match " + std::to_string(GYGAX_PLUGIN_ABI_VERSION));
    if (api->name == nullptr || api->call == nullptr || api->release == nullptr || (api->tool_count > 0 && api->tools == nullptr))
        return fail("incomplete descriptor");
    lib->api = api;
    ExternalModule m;
    m.kind = "plugin";
    m.name = api->name;
    m.version = api->version != nullptr ? api->version : "";
    m.source = resolved;
    if (!validPluginName(m.name)) return fail("plugin name must be 1-32 characters of [a-z0-9_-]");
    if (api->tool_count > kMaxPluginTools) return fail("too many tools");
    for (std::size_t i = 0; i < api->tool_count; ++i) {
        const auto& t = api->tools[i];
        if (t.name == nullptr) return fail("tool without a name");
        ExternalTool tool;
        tool.name = t.name;
        tool.description = t.description != nullptr ? t.description : "";
        if (!validPluginToolName(tool.name)) return fail("invalid tool name '" + tool.name + "'");
        tool.invoke = [lib, api, name = tool.name](const std::string& input) {
            char* out = nullptr;
            const int rc = api->call(name.c_str(), input.c_str(), &out);
            std::string text;
            if (out != nullptr) {
                text.assign(out, ::strnlen(out, kMaxOutput + 1));
                api->release(out);
            }
            if (text.size() > kMaxOutput) throw std::runtime_error("plugin output too large");
            if (rc != 0) throw std::runtime_error(text.empty() ? "plugin tool failed" : text);
            return text;
        };
        m.tools.push_back(std::move(tool));
    }
    m.keepAlive = lib;
    return m;
}

std::optional<ExternalModule> connectExtension(const std::string& spec, std::string* error) {
    auto fail = [&](std::string m) -> std::optional<ExternalModule> {
        if (error != nullptr) *error = "extension " + spec + ": " + std::move(m);
        return std::nullopt;
    };
    std::string base = spec;
    std::string token;
    if (const auto semi = spec.find(';'); semi != std::string::npos) {
        base = spec.substr(0, semi);
        const auto rest = spec.substr(semi + 1);
        if (rest.rfind("token=", 0) != 0) return fail("expected ';token=T' after the URL");
        token = rest.substr(6);
    }
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    if (base.rfind("http://", 0) != 0) return fail("URL must start with http:// (terminate TLS in a local proxy)");
    net::Headers headers{{"Content-Type", "application/json"}};
    if (!token.empty()) headers["Authorization"] = "Bearer " + token;
    auto url = net::Url::parse(base + "/gygax/describe");
    if (!url) return fail("invalid URL");
    net::ClientOptions options;
    options.readTimeout = kExtensionReadTimeout;
    auto resp = net::httpRequest(*url, "GET", {}, headers, options);
    if (!resp.ok || resp.status != net::kHttpStatusOk)
        return fail(resp.ok ? "describe returned HTTP " + std::to_string(resp.status) : resp.error);
    auto doc = json::parse(resp.body);
    if (!doc || !doc->isObject()) return fail("describe did not return a JSON object");
    ExternalModule m;
    m.kind = "ext";
    m.name = doc->getString("name");
    m.version = doc->getString("version");
    m.source = base;
    if (!validPluginName(m.name)) return fail("extension name must be 1-32 characters of [a-z0-9_-]");
    const auto* tools = doc->find("tools");
    if (tools == nullptr || !tools->isArray() || tools->size() > kMaxPluginTools)
        return fail("describe must list at most 128 tools");
    for (const auto& t : tools->asArray()) {
        ExternalTool tool;
        tool.name = t.getString("name");
        tool.description = t.getString("description");
        if (!validPluginToolName(tool.name)) return fail("invalid tool name '" + tool.name + "'");
        tool.invoke = [base, headers, name = tool.name](const std::string& input) {
            json::Value body = json::Value::object();
            body["input"] = input;
            const std::string target = std::string(base).append("/gygax/call/").append(name);
            auto callUrl = net::Url::parse(target);
            if (!callUrl) throw std::runtime_error("invalid extension URL");
            auto r = net::httpRequest(*callUrl, "POST", body.dump(), headers);
            if (!r.ok) throw std::runtime_error("extension unreachable: " + r.error);
            auto out = json::parse(r.body);
            if (r.status != net::kHttpStatusOk || !out || !out->isObject()) {
                std::string message = out && out->isObject() ? out->getString("error") : std::string();
                throw std::runtime_error(message.empty() ? "extension returned HTTP " + std::to_string(r.status) : message);
            }
            auto text = out->getString("output");
            if (text.size() > kMaxOutput) throw std::runtime_error("extension output too large");
            return text;
        };
        m.tools.push_back(std::move(tool));
    }
    return m;
}

}
