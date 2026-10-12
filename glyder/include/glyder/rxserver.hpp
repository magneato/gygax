#pragma once

// A tiny web page served by the headset itself, so a prescription can be typed on a phone instead
// of with a controller. It runs only while the vision panel is open, answers only to the PIN shown
// in the headset, and the prescription never leaves the headset.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace glyder {

class RxServer {
public:
    // `submit` gets the prescription text and returns a message for the page (and the headset).
    using Submit = std::function<std::string(const std::string& text, bool& ok)>;
    RxServer() = default;
    ~RxServer();
    bool start(std::uint16_t port, Submit submit, std::string* error);
    void stop();
    [[nodiscard]] const std::string& pin() const { return pin_; }
    [[nodiscard]] std::string url() const;
    [[nodiscard]] bool running() const { return running_; }

private:
    void serve(int fd);
    int listen_{-1};
    std::uint16_t port_{0};
    std::string pin_;
    Submit submit_;
    std::jthread thread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    int failures_{0};
    double failuresSince_{0};
};

// Decode application/x-www-form-urlencoded.
[[nodiscard]] std::string formValue(const std::string& body, const std::string& key);
[[nodiscard]] std::string htmlEscape(const std::string& s);

} // namespace glyder
