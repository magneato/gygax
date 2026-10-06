#include <gygax/transport/serial.hpp>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include <gygax/core/log.hpp>

namespace gygax::transport {

namespace {

constexpr int kSerialWritePollTimeoutMilliseconds = 1000;
constexpr std::size_t kDrainBufferSize = 64;
constexpr std::size_t kReceiveChunkSize = 4096;
constexpr std::chrono::milliseconds kHangupDrainDelay{5};
constexpr std::chrono::milliseconds kReaderPollInterval{500};

speed_t speedFor(BaudRate baud) {
    switch (baud) {
    case BaudRate::Baud300: return B300;
    case BaudRate::Baud1200: return B1200;
    case BaudRate::Baud2400: return B2400;
    case BaudRate::Baud4800: return B4800;
    case BaudRate::Baud9600: return B9600;
    case BaudRate::Baud19200: return B19200;
    case BaudRate::Baud38400: return B38400;
    case BaudRate::Baud57600: return B57600;
    case BaudRate::Baud115200: return B115200;
    case BaudRate::Baud230400: return B230400;
    }
    return B115200;
}

int lineBit(const std::string& line) {
    if (line == "dtr") return TIOCM_DTR;
    if (line == "rts") return TIOCM_RTS;
    if (line == "cts") return TIOCM_CTS;
    if (line == "dsr") return TIOCM_DSR;
    if (line == "dcd") return TIOCM_CAR;
    if (line == "ri") return TIOCM_RNG;
    return 0;
}

}

std::string describeError(IOResult result) {
    if (result >= 0) return "ok";
    return std::strerror(-result);
}

SerialTransport::SerialTransport() = default;

SerialTransport::~SerialTransport() {
    (void)shutdown();
}

IOResult SerialTransport::initialize() {
    std::lock_guard lock(mutex_);
    if (running_) return 0;
    int fds[2];
    if (::pipe(fds) != 0) return -errno;
    wakeRead_ = fds[0];
    wakeWrite_ = fds[1];
    ::fcntl(wakeRead_, F_SETFL, ::fcntl(wakeRead_, F_GETFL, 0) | O_NONBLOCK);
    running_ = true;
    reader_ = std::thread([this] { readerLoop(); });
    return 0;
}

void SerialTransport::wake() const {
    if (wakeWrite_ >= 0) {
        const char b = 1;
        [[maybe_unused]] const auto rc = ::write(wakeWrite_, &b, 1);
    }
}

IOResult SerialTransport::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return 0;
        running_ = false;
    }
    wake();
    readable_.notify_all();
    if (reader_.joinable()) reader_.join();
    std::lock_guard lock(mutex_);
    for (auto& [h, port] : ports_) {
        if (port.fd >= 0) ::close(port.fd);
    }
    ports_.clear();
    if (wakeRead_ >= 0) ::close(wakeRead_);
    if (wakeWrite_ >= 0) ::close(wakeWrite_);
    wakeRead_ = wakeWrite_ = -1;
    return 0;
}

std::vector<std::string> SerialTransport::enumerateDevices() const {
    std::vector<std::string> out;
    DIR* dir = ::opendir("/dev");
    if (dir == nullptr) return out;
    while (const dirent* e = ::readdir(dir)) {
        const std::string name = e->d_name;
        for (const char* prefix : {"ttyS", "ttyUSB", "ttyACM", "cu.usb", "tty.usb", "ttyAMA", "rfcomm"}) {
            if (name.rfind(prefix, 0) == 0) {
                out.push_back("/dev/" + name);
                break;
            }
        }
    }
    ::closedir(dir);
    std::ranges::sort(out);
    return out;
}

IOResult SerialTransport::open(const std::string& devicePath, TransportHandle& handleOut) {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return -EINVAL;
    }
    const int fd = ::open(devicePath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -errno;
    if (::isatty(fd) != 0) {
        termios tio{};
        if (::tcgetattr(fd, &tio) != 0) {
            const int err = errno;
            ::close(fd);
            return -err;
        }
        ::cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        ::cfsetispeed(&tio, B115200);
        ::cfsetospeed(&tio, B115200);
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
            const int err = errno;
            ::close(fd);
            return -err;
        }
    }
    {
        std::lock_guard lock(mutex_);
        handleOut = nextHandle_++;
        ports_[handleOut].fd = fd;
    }
    wake();
    return 0;
}

IOResult SerialTransport::close(TransportHandle handle) {
    {
        std::lock_guard lock(mutex_);
        auto it = ports_.find(handle);
        if (it == ports_.end()) return -EBADF;
        ::close(it->second.fd);
        ports_.erase(it);
    }
    wake();
    readable_.notify_all();
    return 0;
}

IOResult SerialTransport::configure(TransportHandle handle, BaudRate baud, DataBits dataBits, StopBits stopBits, Parity parity,
                                    FlowControl flow) {
    std::lock_guard lock(mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end()) return -EBADF;
    const int fd = it->second.fd;
    if (::isatty(fd) == 0) return -ENOTTY;
    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) return -errno;
    ::cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    ::cfsetispeed(&tio, speedFor(baud));
    ::cfsetospeed(&tio, speedFor(baud));
    tio.c_cflag &= ~static_cast<tcflag_t>(CSIZE);
    switch (dataBits) {
    case DataBits::Bits5: tio.c_cflag |= CS5; break;
    case DataBits::Bits6: tio.c_cflag |= CS6; break;
    case DataBits::Bits7: tio.c_cflag |= CS7; break;
    case DataBits::Bits8: tio.c_cflag |= CS8; break;
    }
    if (stopBits == StopBits::Bits2)
        tio.c_cflag |= CSTOPB;
    else
        tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);
    tio.c_cflag &= ~static_cast<tcflag_t>(PARENB | PARODD);
    if (parity == Parity::Even) tio.c_cflag |= PARENB;
    if (parity == Parity::Odd) tio.c_cflag |= PARENB | PARODD;
    tio.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
    tio.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
    if (flow == FlowControl::RtsCts) tio.c_cflag |= CRTSCTS;
    if (flow == FlowControl::XonXoff) tio.c_iflag |= IXON | IXOFF;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    return ::tcsetattr(fd, TCSANOW, &tio) == 0 ? 0 : -errno;
}

IOResult SerialTransport::setLine(TransportHandle handle, const std::string& line, bool asserted) {
    const int bit = lineBit(line);
    if (bit == 0 || bit == TIOCM_CTS || bit == TIOCM_DSR || bit == TIOCM_CAR || bit == TIOCM_RNG) return -EINVAL;
    std::lock_guard lock(mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end()) return -EBADF;
    return ::ioctl(it->second.fd, asserted ? TIOCMBIS : TIOCMBIC, &bit) == 0 ? 0 : -errno;
}

IOResult SerialTransport::getLine(TransportHandle handle, const std::string& line, bool& asserted) const {
    const int bit = lineBit(line);
    if (bit == 0) return -EINVAL;
    std::lock_guard lock(mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end()) return -EBADF;
    int status = 0;
    if (::ioctl(it->second.fd, TIOCMGET, &status) != 0) return -errno;
    asserted = (status & bit) != 0;
    return 0;
}

IOResult SerialTransport::write(TransportHandle handle, const std::uint8_t* data, std::size_t length, std::size_t& written) {
    written = 0;
    int fd = -1;
    {
        std::lock_guard lock(mutex_);
        auto it = ports_.find(handle);
        if (it == ports_.end()) return -EBADF;
        fd = it->second.fd;
    }
    while (written < length) {
        const ssize_t n = ::write(fd, data + written, length - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, kSerialWritePollTimeoutMilliseconds) <= 0) return written > 0 ? 0 : -EAGAIN;
            continue;
        }
        return -errno;
    }
    return 0;
}

IOResult SerialTransport::writeAll(TransportHandle handle, const std::string& data, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t sent = 0;
    while (sent < data.size()) {
        std::size_t n = 0;
        const auto rc = write(handle, reinterpret_cast<const std::uint8_t*>(data.data()) + sent, data.size() - sent, n);
        sent += n;
        if (rc < 0 && rc != -EAGAIN) return rc;
        if (sent < data.size() && std::chrono::steady_clock::now() > deadline) return -ETIMEDOUT;
    }
    return 0;
}

IOResult SerialTransport::read(TransportHandle handle, std::uint8_t* buffer, std::size_t capacity, std::size_t& count) {
    std::lock_guard lock(mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end()) return -EBADF;
    count = std::min(capacity, it->second.rx.size());
    std::memcpy(buffer, it->second.rx.data(), count);
    it->second.rx.erase(0, count);
    return 0;
}

IOResult SerialTransport::available(TransportHandle handle, std::size_t& count) const {
    std::lock_guard lock(mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end()) return -EBADF;
    count = it->second.rx.size();
    return 0;
}

IOResult SerialTransport::setCallback(TransportHandle handle, TransferCallback callback) {
    std::lock_guard lock(mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end()) return -EBADF;
    it->second.callback = std::move(callback);
    return 0;
}

IOResult SerialTransport::flushInput(TransportHandle handle) {
    std::lock_guard lock(mutex_);
    auto it = ports_.find(handle);
    if (it == ports_.end()) return -EBADF;
    it->second.rx.clear();
    return 0;
}

IOResult SerialTransport::waitReadable(TransportHandle handle, std::size_t minBytes, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    const bool ok = readable_.wait_for(lock, timeout, [&] {
        auto it = ports_.find(handle);
        return it == ports_.end() || it->second.rx.size() >= minBytes || !running_;
    });
    auto it = ports_.find(handle);
    if (it == ports_.end()) return -EBADF;
    return ok && it->second.rx.size() >= minBytes ? 0 : -ETIMEDOUT;
}

void SerialTransport::readerLoop() {
    std::vector<pollfd> fds;
    std::vector<TransportHandle> handles;
    std::vector<std::uint8_t> chunk(kReceiveChunkSize);
    while (true) {
        fds.clear();
        handles.clear();
        {
            std::lock_guard lock(mutex_);
            if (!running_) return;
            fds.push_back({wakeRead_, POLLIN, 0});
            for (const auto& [h, port] : ports_) {
                fds.push_back({port.fd, POLLIN, 0});
                handles.push_back(h);
            }
        }
        const int rc = ::poll(fds.data(), fds.size(), static_cast<int>(kReaderPollInterval.count()));
        if (rc < 0 && errno != EINTR) return;
        if (rc <= 0) continue;
        if ((fds[0].revents & POLLIN) != 0) {
            char drain[kDrainBufferSize];
            while (::read(wakeRead_, drain, sizeof(drain)) > 0) {
            }
        }
        for (std::size_t i = 1; i < fds.size(); ++i) {
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
            const TransportHandle handle = handles[i - 1];
            const ssize_t n = ::read(fds[i].fd, chunk.data(), chunk.size());
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                if ((fds[i].revents & POLLHUP) != 0) std::this_thread::sleep_for(kHangupDrainDelay);
                continue;
            }
            TransferCallback callback;
            {
                std::lock_guard lock(mutex_);
                auto it = ports_.find(handle);
                if (it == ports_.end()) continue;
                if (it->second.callback) {
                    callback = it->second.callback;
                } else {
                    it->second.rx.append(reinterpret_cast<const char*>(chunk.data()), static_cast<std::size_t>(n));
                    if (it->second.rx.size() > kMaxRx) {
                        const auto excess = it->second.rx.size() - kMaxRx;
                        it->second.rx.erase(0, excess);
                        it->second.dropped += excess;
                    }
                }
            }
            if (callback) callback(handle, chunk.data(), static_cast<std::size_t>(n));
            readable_.notify_all();
        }
    }
}

}
