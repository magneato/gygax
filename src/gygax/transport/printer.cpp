#include <gygax/transport/printer.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <gygax/core/posix.hpp>
#include <charconv>

#include <gygax/core/charconv.hpp>
#include <cstring>

namespace gygax::transport {

namespace {

constexpr std::size_t kTcpSchemeLength = 6;
constexpr unsigned kMaximumTcpPort = 65535;

}

bool PrinterTarget::parse(std::string_view spec, PrinterTarget& out, std::string& error) {
    if (spec.starts_with("tcp://")) {
        spec.remove_prefix(kTcpSchemeLength);
        const auto colon = spec.rfind(':');
        out.kind = Kind::Tcp;
        out.host = std::string(spec.substr(0, colon));
        out.port = kDefaultRawPrinterPort;
        if (colon != std::string_view::npos) {
            unsigned port = 0;
            const auto text = spec.substr(colon + 1);
            auto [ptr, ec] = gygax::fromChars(text.data(), text.data() + text.size(), port);
            if (ec != std::errc() || ptr != text.data() + text.size() || port == 0 || port > kMaximumTcpPort) {
                error = "invalid printer port";
                return false;
            }
            out.port = static_cast<std::uint16_t>(port);
        }
        if (out.host.empty()) {
            error = "missing printer host";
            return false;
        }
        return true;
    }
    if (spec.starts_with("/")) {
        out.kind = Kind::Device;
        out.path = std::string(spec);
        return true;
    }
    error = "printer target must be tcp://host[:port] or a device path";
    return false;
}

namespace {

int connectTcp(const PrinterTarget& t, std::chrono::milliseconds timeout, IOResult& err) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port = std::to_string(t.port);
    if (::getaddrinfo(t.host.c_str(), port.c_str(), &hints, &res) != 0) {
        err = -EHOSTUNREACH;
        return -1;
    }
    int fd = -1;
    err = -ECONNREFUSED;
    for (addrinfo* ai = res; ai != nullptr && fd < 0; ai = ai->ai_next) {
        const int s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK);
        suppressSigpipe(s);
        int rc = ::connect(s, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd p{s, POLLOUT, 0};
            if (::poll(&p, 1, static_cast<int>(timeout.count())) > 0) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                ::getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len);
                rc = soerr == 0 ? 0 : -1;
                if (soerr != 0) err = -soerr;
            } else {
                err = -ETIMEDOUT;
                rc = -1;
            }
        }
        if (rc == 0) {
            fd = s;
            err = 0;
        } else {
            ::close(s);
        }
    }
    ::freeaddrinfo(res);
    return fd;
}

IOResult sendAll(int fd, std::string_view data, std::chrono::milliseconds timeout) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, static_cast<int>(timeout.count())) <= 0) return -ETIMEDOUT;
            continue;
        }
        return -errno;
    }
    return 0;
}

IOResult writeAllFd(int fd, std::string_view data, std::chrono::milliseconds timeout) {
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, static_cast<int>(timeout.count())) <= 0) return -ETIMEDOUT;
            continue;
        }
        return -errno;
    }
    return 0;
}

}

IOResult RawPrinter::print(std::string_view data) const {
    if (target_.kind == PrinterTarget::Kind::Device) {
        const int fd = ::open(target_.path.c_str(), O_WRONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) return -errno;
        const auto rc = writeAllFd(fd, data, timeout_);
        ::close(fd);
        return rc;
    }
    IOResult err = 0;
    const int fd = connectTcp(target_, timeout_, err);
    if (fd < 0) return err;
    const auto rc = sendAll(fd, data, timeout_);
    ::shutdown(fd, SHUT_WR);
    ::close(fd);
    return rc;
}

std::string RawPrinter::wrapPjl(std::string_view jobName, std::string_view data) {
    std::string out = "\x1b%-12345X@PJL JOB NAME=\"";
    for (const char c : jobName) {
        if (c != '"' && c != '\r' && c != '\n') out.push_back(c);
    }
    out += "\"\r\n";
    out.append(data);
    out += "\r\n@PJL EOJ\r\n\x1b%-12345X";
    return out;
}

IOResult RawPrinter::printPjl(std::string_view jobName, std::string_view data) const {
    return print(wrapPjl(jobName, data));
}

IOResult RawPrinter::query(std::string_view request, std::string& response) const {
    if (target_.kind != PrinterTarget::Kind::Tcp) return -ENOTSUP;
    IOResult err = 0;
    const int fd = connectTcp(target_, timeout_, err);
    if (fd < 0) return err;
    if (const auto rc = sendAll(fd, request, timeout_); rc != 0) {
        ::close(fd);
        return rc;
    }
    response.clear();
    char buf[1024];
    while (true) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(timeout_.count())) <= 0) break;
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, static_cast<std::size_t>(n));
        if (response.find('\f') != std::string::npos) break;
    }
    ::close(fd);
    return 0;
}

}
