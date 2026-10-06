#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <gygax/transport/base.hpp>

namespace gygax::transport {

inline constexpr const char* kDefaultSysfsGpioRoot = "/sys/class/gpio";
inline constexpr std::chrono::milliseconds kDefaultGpioPollInterval{5};

class SysfsGpioPort final : public GpioPort {
public:
    explicit SysfsGpioPort(std::string root = kDefaultSysfsGpioRoot, std::chrono::milliseconds pollInterval = kDefaultGpioPollInterval);
    ~SysfsGpioPort() override;
    SysfsGpioPort(const SysfsGpioPort&) = delete;
    SysfsGpioPort& operator=(const SysfsGpioPort&) = delete;

    [[nodiscard]] IOResult initialize() override;
    [[nodiscard]] IOResult shutdown() override;
    [[nodiscard]] IOResult configure(std::uint32_t pin, GpioMode mode) override;
    [[nodiscard]] IOResult read(std::uint32_t pin, GpioLevel& level) const override;
    [[nodiscard]] IOResult write(std::uint32_t pin, GpioLevel level) override;
    [[nodiscard]] IOResult watch(std::uint32_t pin, GpioCallback callback) override;
    [[nodiscard]] IOResult unwatch(std::uint32_t pin) override;

    [[nodiscard]] GpioMode mode(std::uint32_t pin) const;

private:
    struct Pin {
        GpioMode mode = GpioMode::Input;
        bool exportedByUs = false;
        GpioCallback callback;
        int last = -1;
    };

    [[nodiscard]] std::string pinPath(std::uint32_t pin) const;
    [[nodiscard]] IOResult readValue(std::uint32_t pin, GpioLevel& level) const;
    void pollLoop();

    std::string root_;
    std::chrono::milliseconds pollInterval_;
    mutable std::mutex mutex_;
    std::map<std::uint32_t, Pin> pins_;
    std::thread poller_;
    std::condition_variable wake_;
    bool running_ = false;
};

}
