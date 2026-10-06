#include <gygax/transport/gpio.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>

namespace gygax::transport {

namespace {

constexpr int kPinExportPollAttempts = 100;
constexpr useconds_t kPinExportPollIntervalMicroseconds = 5000;

IOResult writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    if (!out) return -errno;
    out << text;
    out.flush();
    return out ? 0 : -EIO;
}

bool exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}

SysfsGpioPort::SysfsGpioPort(std::string root, std::chrono::milliseconds pollInterval)
    : root_(std::move(root)), pollInterval_(pollInterval) {}

SysfsGpioPort::~SysfsGpioPort() {
    (void)shutdown();
}

std::string SysfsGpioPort::pinPath(std::uint32_t pin) const {
    return root_ + "/gpio" + std::to_string(pin);
}

IOResult SysfsGpioPort::initialize() {
    std::lock_guard lock(mutex_);
    if (running_) return 0;
    if (!exists(root_)) return -ENOENT;
    running_ = true;
    poller_ = std::thread([this] { pollLoop(); });
    return 0;
}

IOResult SysfsGpioPort::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return 0;
        running_ = false;
    }
    wake_.notify_all();
    if (poller_.joinable()) poller_.join();
    std::lock_guard lock(mutex_);
    for (auto& [pin, state] : pins_) {
        if (state.exportedByUs) (void)writeFile(root_ + "/unexport", std::to_string(pin));
    }
    pins_.clear();
    return 0;
}

IOResult SysfsGpioPort::configure(std::uint32_t pin, GpioMode mode) {
    std::lock_guard lock(mutex_);
    if (!running_) return -EINVAL;
    bool exportedNow = false;
    if (!exists(pinPath(pin))) {
        if (const auto rc = writeFile(root_ + "/export", std::to_string(pin)); rc < 0) return rc;
        exportedNow = true;
        for (int i = 0; i < kPinExportPollAttempts && !exists(pinPath(pin)); ++i)
            ::usleep(kPinExportPollIntervalMicroseconds);
        if (!exists(pinPath(pin))) return -ENODEV;
    }
    if (const auto rc = writeFile(pinPath(pin) + "/direction", mode == GpioMode::Output ? "out" : "in"); rc < 0) return rc;
    auto& state = pins_[pin];
    state.mode = mode;
    state.exportedByUs = state.exportedByUs || exportedNow;
    GpioLevel level{};
    state.last = readValue(pin, level) == 0 ? static_cast<int>(level) : -1;
    return 0;
}

IOResult SysfsGpioPort::readValue(std::uint32_t pin, GpioLevel& level) const {
    std::ifstream in(pinPath(pin) + "/value");
    if (!in) return -errno;
    char c = 0;
    in >> c;
    if (c != '0' && c != '1') return -EIO;
    level = c == '1' ? GpioLevel::High : GpioLevel::Low;
    return 0;
}

IOResult SysfsGpioPort::read(std::uint32_t pin, GpioLevel& level) const {
    std::lock_guard lock(mutex_);
    if (!pins_.contains(pin)) return -EINVAL;
    return readValue(pin, level);
}

IOResult SysfsGpioPort::write(std::uint32_t pin, GpioLevel level) {
    std::lock_guard lock(mutex_);
    auto it = pins_.find(pin);
    if (it == pins_.end()) return -EINVAL;
    if (it->second.mode != GpioMode::Output) return -EPERM;
    if (const auto rc = writeFile(pinPath(pin) + "/value", level == GpioLevel::High ? "1" : "0"); rc < 0) return rc;
    it->second.last = static_cast<int>(level);
    return 0;
}

IOResult SysfsGpioPort::watch(std::uint32_t pin, GpioCallback callback) {
    std::lock_guard lock(mutex_);
    auto it = pins_.find(pin);
    if (it == pins_.end()) return -EINVAL;
    it->second.callback = std::move(callback);
    GpioLevel level{};
    if (readValue(pin, level) == 0) it->second.last = static_cast<int>(level);
    return 0;
}

IOResult SysfsGpioPort::unwatch(std::uint32_t pin) {
    std::lock_guard lock(mutex_);
    auto it = pins_.find(pin);
    if (it == pins_.end()) return -EINVAL;
    it->second.callback = nullptr;
    return 0;
}

GpioMode SysfsGpioPort::mode(std::uint32_t pin) const {
    std::lock_guard lock(mutex_);
    auto it = pins_.find(pin);
    return it == pins_.end() ? GpioMode::Input : it->second.mode;
}

void SysfsGpioPort::pollLoop() {
    std::unique_lock lock(mutex_);
    while (running_) {
        struct Event {
            std::uint32_t pin;
            GpioLevel level;
            GpioCallback callback;
        };
        std::vector<Event> events;
        for (auto& [pin, state] : pins_) {
            if (!state.callback) continue;
            GpioLevel level{};
            if (readValue(pin, level) != 0) continue;
            if (static_cast<int>(level) != state.last) {
                state.last = static_cast<int>(level);
                events.push_back({pin, level, state.callback});
            }
        }
        lock.unlock();
        for (const auto& e : events) e.callback(e.pin, e.level, nowNs());
        lock.lock();
        wake_.wait_for(lock, pollInterval_, [this] { return !running_; });
    }
}

}
