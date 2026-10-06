#pragma once

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include <gygax/core/json.hpp>
#include <gygax/net/http.hpp>

namespace gygax::support {

inline constexpr auto kPtyWriteRetryInterval = std::chrono::milliseconds(1);
inline constexpr auto kDefaultWaitTimeout = std::chrono::milliseconds(3000);
inline constexpr auto kWaitPollInterval = std::chrono::milliseconds(2);

struct Pty {
    int master = -1;
    std::string slavePath;

    Pty() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0) return;
        if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            ::close(master);
            master = -1;
            return;
        }
        if (const char* name = ::ptsname(master)) slavePath = name;
        ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL, 0) | O_NONBLOCK);
    }

    ~Pty() {
        if (master >= 0) ::close(master);
    }

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    [[nodiscard]] bool valid() const { return master >= 0 && !slavePath.empty(); }

    void write(const std::string& data) const {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = ::write(master, data.data() + sent, data.size() - sent);
            if (n > 0)
                sent += static_cast<std::size_t>(n);
            else if (n < 0 && errno == EAGAIN)
                std::this_thread::sleep_for(kPtyWriteRetryInterval);
            else
                break;
        }
    }

    std::string readAvailable() const {
        std::string out;
        char buf[512];
        while (true) {
            const ssize_t n = ::read(master, buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, static_cast<std::size_t>(n));
        }
        return out;
    }
};

inline net::ClientResponse get(const net::Server& server, const std::string& path, const net::Headers& headers = {}) {
    auto url = net::Url::parse("http://127.0.0.1:" + std::to_string(server.port()) + path);
    return net::httpRequest(*url, "GET", {}, headers);
}

inline net::ClientResponse post(const net::Server& server, const std::string& path, const std::string& body,
                                const net::Headers& headers = {}) {
    auto url = net::Url::parse("http://127.0.0.1:" + std::to_string(server.port()) + path);
    return net::httpRequest(*url, "POST", body, headers);
}

inline bool waitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = kDefaultWaitTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(kWaitPollInterval);
    }
    return predicate();
}

}
