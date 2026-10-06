#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <gygax/core/json.hpp>

namespace gygax::log {

enum class Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };
enum class Format : int { Text = 0, Json = 1 };

[[nodiscard]] constexpr std::string_view levelName(Level level) {
    switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Off: return "off";
    }
    return "info";
}

[[nodiscard]] inline Level parseLevel(std::string_view text, Level fallback = Level::Info) {
    for (int i = 0; i <= static_cast<int>(Level::Off); ++i) {
        if (levelName(static_cast<Level>(i)) == text) return static_cast<Level>(i);
    }
    return fallback;
}

using Sink = std::function<void(Level, std::string_view)>;

namespace detail {

struct State {
    State() {
        if (const char* env = std::getenv("GYGAX_LOG_LEVEL")) level.store(static_cast<int>(parseLevel(env)));
        if (const char* env = std::getenv("GYGAX_LOG_FORMAT")) {
            if (std::string_view(env) == "json") format.store(static_cast<int>(Format::Json));
        }
    }
    std::atomic<int> level{static_cast<int>(Level::Info)};
    std::atomic<int> format{static_cast<int>(Format::Text)};
    std::mutex mutex;
    Sink sink;
};

inline State& state() {
    static State instance;
    return instance;
}

inline std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - secs).count();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::format("{}.{:03}Z", buf, millis);
}

inline void emit(Level level, std::string_view component, std::string_view message) {
    auto& st = state();
    std::string line;
    if (st.format.load() == static_cast<int>(Format::Json)) {
        json::Value entry = json::Value::object();
        entry["ts"] = timestamp();
        entry["level"] = std::string(levelName(level));
        entry["component"] = std::string(component);
        entry["msg"] = std::string(message);
        line = entry.dump();
    } else {
        line = std::format("{} {:<5} [{}] {}", timestamp(), levelName(level), component, message);
    }
    std::lock_guard lock(st.mutex);
    if (st.sink) {
        st.sink(level, line);
        return;
    }
    std::fprintf(stderr, "%s\n", line.c_str());
}

}

inline void setLevel(Level level) {
    detail::state().level.store(static_cast<int>(level));
}
inline void setFormat(Format format) {
    detail::state().format.store(static_cast<int>(format));
}
[[nodiscard]] inline Level level() {
    return static_cast<Level>(detail::state().level.load());
}

inline void setSink(Sink sink) {
    auto& st = detail::state();
    std::lock_guard lock(st.mutex);
    st.sink = std::move(sink);
}

[[nodiscard]] inline bool enabled(Level l) {
    return static_cast<int>(l) >= detail::state().level.load();
}

template <typename... Args> void write(Level l, std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    if (!enabled(l)) return;
    detail::emit(l, component, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void debug(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, component, fmt, std::forward<Args>(args)...);
}

template <typename... Args> void info(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, component, fmt, std::forward<Args>(args)...);
}

template <typename... Args> void warn(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, component, fmt, std::forward<Args>(args)...);
}

template <typename... Args> void error(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, component, fmt, std::forward<Args>(args)...);
}

}
