#include <gygax/net/http.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <random>

#include <gygax/core/json.hpp>
#include <gygax/core/log.hpp>
#include <gygax/core/posix.hpp>

namespace gygax::net {

namespace {

constexpr std::chrono::seconds kLingeringCloseTimeout{2};
constexpr std::chrono::milliseconds kLingeringCloseReadTimeout{200};
constexpr std::size_t kMaximumLingerDrainBytes = 16ULL * 1024 * 1024;
constexpr std::size_t kMaximumRequestIdLength = 64;
constexpr int kListenBacklog = 256;
constexpr int kAcceptPollIntervalMilliseconds = 500;
constexpr std::chrono::milliseconds kOverloadResponseTimeout{200};
constexpr std::size_t kInitialResponseBufferCapacity = 4096;
constexpr std::size_t kResponseReadChunkSize = 8192;
constexpr std::size_t kMaximumResponseHeaderBufferBytes = 64 * 1024;
constexpr std::chrono::milliseconds kInitialResponseReadTimeout{2000};
constexpr int kMinimumHttpStatusCode = 100;
constexpr int kMaximumHttpStatusCode = 599;
constexpr int kHttpSuccessStatusClassStart = 200;
constexpr int kHttpRedirectStatusClassStart = 300;
constexpr int kHttpClientErrorStatusClassStart = 400;
constexpr int kHttpServerErrorStatusClassStart = 500;
constexpr std::uint32_t kMaximumPortNumber = 65535;

char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string toLower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), lower);
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void closeFd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

enum class WaitResult { Ready, Timeout, Error };

WaitResult waitFor(int fd, short events, std::chrono::milliseconds timeout) {
    pollfd p{fd, events, 0};
    while (true) {
        const int rc = ::poll(&p, 1, static_cast<int>(timeout.count()));
        if (rc > 0) return (p.revents & (POLLERR | POLLNVAL)) != 0 && (p.revents & events) == 0 ? WaitResult::Error : WaitResult::Ready;
        if (rc == 0) return WaitResult::Timeout;
        if (errno != EINTR) return WaitResult::Error;
    }
}

bool sendAll(int fd, std::string_view data, std::chrono::milliseconds timeout) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (waitFor(fd, POLLOUT, timeout) != WaitResult::Ready) return false;
            continue;
        }
        return false;
    }
    return true;
}

struct ReadResult {
    enum Kind { Data, Eof, Timeout, Error } kind;
    std::size_t size = 0;
};

ReadResult readSome(int fd, char* buffer, std::size_t capacity, std::chrono::milliseconds timeout) {
    while (true) {
        const ssize_t n = ::recv(fd, buffer, capacity, 0);
        if (n > 0) return {ReadResult::Data, static_cast<std::size_t>(n)};
        if (n == 0) return {ReadResult::Eof, 0};
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const auto w = waitFor(fd, POLLIN, timeout);
            if (w == WaitResult::Timeout) return {ReadResult::Timeout, 0};
            if (w == WaitResult::Error) return {ReadResult::Error, 0};
            continue;
        }
        return {ReadResult::Error, 0};
    }
}

void lingeringClose(int fd) {
    ::shutdown(fd, SHUT_WR);
    std::array<char, 16384> sink{};
    std::size_t drained = 0;
    const auto deadline = std::chrono::steady_clock::now() + kLingeringCloseTimeout;
    while (drained < kMaximumLingerDrainBytes && std::chrono::steady_clock::now() < deadline) {
        const auto rr = readSome(fd, sink.data(), sink.size(), kLingeringCloseReadTimeout);
        if (rr.kind != ReadResult::Data) break;
        drained += rr.size;
    }
}

void appendHeaderLine(std::string& wire, const std::string& name, const std::string& value) {
    wire += name;
    wire += ": ";
    wire += value;
    wire += "\r\n";
}

std::vector<std::string> splitPath(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        if (end > start) parts.emplace_back(path.substr(start, end - start));
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return parts;
}

Query parseQuery(std::string_view raw) {
    Query out;
    std::size_t start = 0;
    while (start <= raw.size()) {
        const auto amp = raw.find('&', start);
        const auto end = amp == std::string_view::npos ? raw.size() : amp;
        const auto pair = raw.substr(start, end - start);
        if (!pair.empty()) {
            const auto eq = pair.find('=');
            std::string key(pair.substr(0, eq));
            std::string value = eq == std::string_view::npos ? std::string() : std::string(pair.substr(eq + 1));
            std::ranges::replace(key, '+', ' ');
            std::ranges::replace(value, '+', ' ');
            out[urlDecode(key)] = urlDecode(value);
        }
        if (amp == std::string_view::npos) break;
        start = amp + 1;
    }
    return out;
}

bool validRequestId(std::string_view id) {
    if (id.empty() || id.size() > kMaximumRequestIdLength) return false;
    return std::ranges::all_of(id,
                               [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.'; });
}

std::string idPrefix() {
    static const std::string prefix = [] {
        std::random_device rd;
        return std::format("{:08x}", rd());
    }();
    return prefix;
}

class SocketWriter final : public ChunkWriter {
public:
    SocketWriter(int fd, std::chrono::milliseconds timeout, std::uint64_t& bytes, int status, Headers headers)
        : fd_(fd), timeout_(timeout), bytes_(bytes), status_(status), headers_(std::move(headers)) {}

    bool write(std::string_view data) override {
        if (aborted_) return false;
        if (!commit()) return false;
        if (data.empty()) return true;
        const std::string frame = std::format("{:x}\r\n", data.size());
        if (!sendAll(fd_, frame, timeout_) || !sendAll(fd_, data, timeout_) || !sendAll(fd_, "\r\n", timeout_)) {
            aborted_ = true;
            return false;
        }
        bytes_ += frame.size() + data.size() + 2;
        return true;
    }

    void setStatus(int status) override {
        if (!sent_) status_ = status;
    }

    void setHeader(const std::string& name, const std::string& value) override {
        if (!sent_) headers_[name] = value;
    }

    [[nodiscard]] bool headersSent() const override { return sent_; }
    [[nodiscard]] bool aborted() const override { return aborted_; }
    [[nodiscard]] int status() const { return status_; }

    bool finish() {
        if (aborted_ || !commit()) return false;
        if (!sendAll(fd_, "0\r\n\r\n", timeout_)) {
            aborted_ = true;
            return false;
        }
        bytes_ += 5;
        return true;
    }

private:
    bool commit() {
        if (sent_) return true;
        sent_ = true;
        std::string wire = std::format("HTTP/1.1 {} {}\r\n", status_, statusText(status_));
        for (const auto& [k, v] : headers_) appendHeaderLine(wire, k, v);
        wire += "\r\n";
        if (!sendAll(fd_, wire, timeout_)) {
            aborted_ = true;
            return false;
        }
        bytes_ += wire.size();
        return true;
    }

    int fd_;
    std::chrono::milliseconds timeout_;
    std::uint64_t& bytes_;
    int status_;
    Headers headers_;
    bool sent_ = false;
    bool aborted_ = false;
};

struct ParsedHead {
    std::string firstLine;
    Headers headers;
};

std::optional<ParsedHead> parseHead(std::string_view head, std::size_t maxHeaders = 128) {
    ParsedHead out;
    std::size_t pos = 0;
    bool first = true;
    std::size_t count = 0;
    while (pos < head.size()) {
        auto eol = head.find("\r\n", pos);
        if (eol == std::string_view::npos) eol = head.size();
        const auto line = head.substr(pos, eol - pos);
        pos = eol + 2;
        if (first) {
            out.firstLine = std::string(line);
            first = false;
            continue;
        }
        if (line.empty()) break;
        if (line.front() == ' ' || line.front() == '\t') return std::nullopt;
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) return std::nullopt;
        const auto name = line.substr(0, colon);
        if (name.find_first_of(" \t") != std::string_view::npos) return std::nullopt;
        if (++count > maxHeaders) return std::nullopt;
        auto value = std::string(trim(line.substr(colon + 1)));
        auto [it, inserted] = out.headers.try_emplace(std::string(name), value);
        if (!inserted) it->second += ", " + value;
    }
    if (out.firstLine.empty()) return std::nullopt;
    return out;
}

}

bool CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const {
    return std::ranges::lexicographical_compare(a, b, [](char x, char y) { return lower(x) < lower(y); });
}

std::string urlDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hexValue(text[i + 1]) >= 0 && hexValue(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hexValue(text[i + 1]) * 16 + hexValue(text[i + 2])));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string urlEncode(std::string_view text) {
    std::string out;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(raw);
        } else {
            out += std::format("%{:02X}", c);
        }
    }
    return out;
}

std::string statusText(int status) {
    switch (status) {
    case kHttpStatusContinue: return "Continue";
    case kHttpStatusOk: return "OK";
    case kHttpStatusCreated: return "Created";
    case kHttpStatusAccepted: return "Accepted";
    case kHttpStatusNoContent: return "No Content";
    case kHttpStatusMovedPermanently: return "Moved Permanently";
    case kHttpStatusFound: return "Found";
    case kHttpStatusNotModified: return "Not Modified";
    case kHttpStatusBadRequest: return "Bad Request";
    case kHttpStatusUnauthorized: return "Unauthorized";
    case kHttpStatusForbidden: return "Forbidden";
    case kHttpStatusNotFound: return "Not Found";
    case kHttpStatusMethodNotAllowed: return "Method Not Allowed";
    case kHttpStatusRequestTimeout: return "Request Timeout";
    case kHttpStatusConflict: return "Conflict";
    case kHttpStatusLengthRequired: return "Length Required";
    case kHttpStatusPayloadTooLarge: return "Payload Too Large";
    case kHttpStatusUnsupportedMediaType: return "Unsupported Media Type";
    case kHttpStatusUnprocessableEntity: return "Unprocessable Entity";
    case kHttpStatusTooManyRequests: return "Too Many Requests";
    case kHttpStatusRequestHeaderFieldsTooLarge: return "Request Header Fields Too Large";
    case kHttpStatusInternalServerError: return "Internal Server Error";
    case kHttpStatusNotImplemented: return "Not Implemented";
    case kHttpStatusBadGateway: return "Bad Gateway";
    case kHttpStatusServiceUnavailable: return "Service Unavailable";
    case kHttpStatusGatewayTimeout: return "Gateway Timeout";
    default: return "Status";
    }
}

bool constantTimeEquals(std::string_view a, std::string_view b) {
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    const std::size_t n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(x ^ y);
    }
    return diff == 0;
}

std::string Request::header(const std::string& name, const std::string& fallback) const {
    auto it = headers.find(name);
    return it == headers.end() ? fallback : it->second;
}

std::string Request::queryValue(const std::string& name, const std::string& fallback) const {
    auto it = query.find(name);
    return it == query.end() ? fallback : it->second;
}

Response Response::json(int status, std::string body) {
    Response r;
    r.status = status;
    r.headers["Content-Type"] = "application/json";
    r.body = std::move(body);
    return r;
}

Response Response::text(int status, std::string body, std::string contentType) {
    Response r;
    r.status = status;
    r.headers["Content-Type"] = std::move(contentType);
    r.body = std::move(body);
    return r;
}

Response Response::error(int status, std::string_view code, std::string_view message) {
    json::Value err = json::Value::object();
    err["error"]["code"] = std::string(code);
    err["error"]["message"] = std::string(message);
    err["error"]["status"] = status;
    return json(status, err.dump());
}

Response Response::streaming(int status, std::string contentType, std::function<void(ChunkWriter&)> producer) {
    Response r;
    r.status = status;
    r.headers["Content-Type"] = std::move(contentType);
    r.headers["Cache-Control"] = "no-cache";
    r.stream = std::move(producer);
    return r;
}

Server::Server(ServerOptions options) : options_(std::move(options)) {}

Server::~Server() {
    stop();
}

void Server::route(std::string method, const std::string& pattern, Handler handler) {
    Route r;
    r.method = std::move(method);
    r.segments = splitPath(pattern);
    if (!r.segments.empty() && r.segments.back() == "*") {
        r.wildcard = true;
        r.segments.pop_back();
    }
    r.handler = std::move(handler);
    routes_.push_back(std::move(r));
}

void Server::intercept(Interceptor interceptor) {
    interceptors_.push_back(std::move(interceptor));
}

ServerStats Server::stats() const {
    std::lock_guard lock(statsMutex_);
    return stats_;
}

bool Server::start(std::string* error) {
    if (running_.load()) return true;
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        closeFd(listenFd_);
        closeFd(wakeRead_);
        closeFd(wakeWrite_);
        return false;
    };

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    addrinfo* res = nullptr;
    const std::string portText = std::to_string(options_.port);
    const char* hostArg = options_.host.empty() || options_.host == "*" ? nullptr : options_.host.c_str();
    if (const int rc = ::getaddrinfo(hostArg, portText.c_str(), &hints, &res); rc != 0) {
        return fail(std::format("cannot resolve bind address '{}': {}", options_.host, ::gai_strerror(rc)));
    }

    std::string lastError = "no usable address";
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            lastError = std::strerror(errno);
            continue;
        }
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (ai->ai_family == AF_INET6) {
            const int v6only = hostArg == nullptr ? 0 : 1;
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        }
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 || ::listen(fd, kListenBacklog) != 0) {
            lastError = std::strerror(errno);
            ::close(fd);
            continue;
        }
        listenFd_ = fd;
        break;
    }
    ::freeaddrinfo(res);
    if (listenFd_ < 0) return fail(std::format("cannot listen on {}:{}: {}", options_.host, options_.port, lastError));
    setNonBlocking(listenFd_);

    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        boundPort_ = bound.ss_family == AF_INET6 ? ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port)
                                                 : ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
    }

    int pipefd[2];
    if (::pipe(pipefd) != 0) return fail(std::string("pipe: ") + std::strerror(errno));
    wakeRead_ = pipefd[0];
    wakeWrite_ = pipefd[1];
    setNonBlocking(wakeRead_);

    running_.store(true);
    const std::size_t count = std::max<std::size_t>(1, options_.workers);
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) workers_.emplace_back([this] { workerLoop(); });
    acceptor_ = std::thread([this] { acceptLoop(); });
    log::info("http", "listening on {}:{} ({} workers)", options_.host, boundPort_, count);
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    if (wakeWrite_ >= 0) {
        const char b = 1;
        [[maybe_unused]] const auto rc = ::write(wakeWrite_, &b, 1);
    }
    queueReady_.notify_all();
    if (acceptor_.joinable()) acceptor_.join();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
    {
        std::lock_guard lock(queueMutex_);
        for (auto& p : queue_) ::close(p.fd);
        queue_.clear();
    }
    closeFd(listenFd_);
    closeFd(wakeRead_);
    closeFd(wakeWrite_);
    log::info("http", "stopped");
}

void Server::acceptLoop() {
    while (running_.load()) {
        pollfd fds[2] = {{listenFd_, POLLIN, 0}, {wakeRead_, POLLIN, 0}};
        const int rc = ::poll(fds, 2, kAcceptPollIntervalMilliseconds);
        if (rc < 0 && errno != EINTR) break;
        if (rc <= 0) continue;
        if ((fds[1].revents & POLLIN) != 0) break;
        if ((fds[0].revents & POLLIN) == 0) continue;

        while (true) {
            sockaddr_storage peer{};
            socklen_t plen = sizeof(peer);
            int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (fd < 0) break;
            setNonBlocking(fd);
            suppressSigpipe(fd);
            const int on = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

            char host[INET6_ADDRSTRLEN] = "unknown";
            std::uint16_t port = 0;
            if (peer.ss_family == AF_INET) {
                auto* a = reinterpret_cast<sockaddr_in*>(&peer);
                ::inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
                port = ntohs(a->sin_port);
            } else if (peer.ss_family == AF_INET6) {
                auto* a = reinterpret_cast<sockaddr_in6*>(&peer);
                ::inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
                port = ntohs(a->sin6_port);
            }
            std::string remote = std::format("{}:{}", host, port);

            bool queued = false;
            {
                std::lock_guard lock(queueMutex_);
                if (queue_.size() < options_.maxQueuedConnections) {
                    queue_.push_back({fd, std::move(remote)});
                    queued = true;
                }
            }
            {
                std::lock_guard lock(statsMutex_);
                ++stats_.accepted;
                if (!queued) ++stats_.rejectedOverload;
            }
            if (queued) {
                queueReady_.notify_one();
            } else {
                constexpr std::string_view busy =
                    "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nRetry-After: 1\r\nContent-Length: 0\r\n\r\n";
                sendAll(fd, busy, kOverloadResponseTimeout);
                ::close(fd);
            }
        }
    }
}

void Server::workerLoop() {
    while (true) {
        Pending job{-1, {}};
        {
            std::unique_lock lock(queueMutex_);
            queueReady_.wait(lock, [this] { return !queue_.empty() || !running_.load(); });
            if (queue_.empty()) {
                if (!running_.load()) return;
                continue;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        serve(job.fd, job.remote);
        ::close(job.fd);
    }
}

bool Server::matchRoute(const Route& route, const std::vector<std::string>& parts, Request& request) const {
    if (route.wildcard ? parts.size() < route.segments.size() : parts.size() != route.segments.size()) return false;
    std::map<std::string, std::string> params;
    for (std::size_t i = 0; i < route.segments.size(); ++i) {
        const auto& seg = route.segments[i];
        if (!seg.empty() && seg.front() == ':') {
            params[seg.substr(1)] = urlDecode(parts[i]);
        } else if (seg != parts[i]) {
            return false;
        }
    }
    request.params = std::move(params);
    return true;
}

Response Server::dispatch(Request& request) {
    if (request.method == "OPTIONS" && !options_.corsOrigin.empty()) {
        Response r;
        r.status = kHttpStatusNoContent;
        return r;
    }
    for (const auto& interceptor : interceptors_) {
        if (auto early = interceptor(request)) return std::move(*early);
    }
    const auto parts = splitPath(request.path);
    std::vector<std::string> allowed;
    for (const auto& route : routes_) {
        Request probe;
        if (!matchRoute(route, parts, probe)) continue;
        if (route.method == request.method || (request.method == "HEAD" && route.method == "GET")) {
            request.params = std::move(probe.params);
            try {
                return route.handler(request);
            } catch (const std::exception& e) {
                log::error("http", "handler for {} {} threw: {}", request.method, request.path, e.what());
                return Response::error(kHttpStatusInternalServerError, "internal_error", "internal server error");
            } catch (...) {
                log::error("http", "handler for {} {} threw a non-standard exception", request.method, request.path);
                return Response::error(kHttpStatusInternalServerError, "internal_error", "internal server error");
            }
        }
        allowed.push_back(route.method);
    }
    if (!allowed.empty()) {
        std::ranges::sort(allowed);
        allowed.erase(std::unique(allowed.begin(), allowed.end()), allowed.end());
        std::string list;
        for (const auto& m : allowed) list += (list.empty() ? "" : ", ") + m;
        Response r =
            Response::error(kHttpStatusMethodNotAllowed, "method_not_allowed", "method not allowed for this resource");
        r.headers["Allow"] = list;
        return r;
    }
    return Response::error(kHttpStatusNotFound, "not_found", "no such resource");
}

void Server::serve(int fd, const std::string& remote) {
    const auto started = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(statsMutex_);
        ++stats_.inFlight;
    }
    std::uint64_t bytesIn = 0;
    std::uint64_t bytesOut = 0;
    int finalStatus = 0;
    bool countedRequest = false;
    bool bad = false;
    bool lingering = false;

    auto finish = [&] {
        if (lingering) lingeringClose(fd);
        std::lock_guard lock(statsMutex_);
        --stats_.inFlight;
        stats_.bytesIn += bytesIn;
        stats_.bytesOut += bytesOut;
        if (countedRequest) ++stats_.requests;
        if (bad) ++stats_.badRequests;
        if (finalStatus >= kHttpSuccessStatusClassStart && finalStatus < kHttpRedirectStatusClassStart)
            ++stats_.responses2xx;
        else if (finalStatus >= kHttpRedirectStatusClassStart && finalStatus < kHttpClientErrorStatusClassStart)
            ++stats_.responses3xx;
        else if (finalStatus >= kHttpClientErrorStatusClassStart && finalStatus < kHttpServerErrorStatusClassStart)
            ++stats_.responses4xx;
        else if (finalStatus >= kHttpServerErrorStatusClassStart)
            ++stats_.responses5xx;
    };

    auto reject = [&](int status, std::string_view code, std::string_view message) {
        Response r = Response::error(status, code, message);
        std::string wire =
            std::format("HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\nServer: {}\r\n\r\n",
                        status, statusText(status), r.body.size(), options_.serverName);
        wire += r.body;
        sendAll(fd, wire, options_.writeTimeout);
        bytesOut += wire.size();
        finalStatus = status;
        bad = true;
        lingering = true;
    };

    std::string buffer;
    buffer.reserve(kInitialResponseBufferCapacity);
    std::array<char, kResponseReadChunkSize> chunk{};
    std::size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos) {
        if (buffer.size() > options_.maxHeaderBytes) {
            reject(kHttpStatusRequestHeaderFieldsTooLarge, "header_too_large", "request headers too large");
            finish();
            return;
        }
        const auto rr = readSome(fd, chunk.data(), chunk.size(), options_.readTimeout);
        if (rr.kind == ReadResult::Timeout) {
            if (!buffer.empty())
                reject(kHttpStatusRequestTimeout, "request_timeout", "timed out waiting for request");
            finish();
            return;
        }
        if (rr.kind != ReadResult::Data) {
            finish();
            return;
        }
        bytesIn += rr.size;
        buffer.append(chunk.data(), rr.size);
        headEnd = buffer.find("\r\n\r\n");
    }
    if (headEnd > options_.maxHeaderBytes) {
        reject(kHttpStatusRequestHeaderFieldsTooLarge, "header_too_large", "request headers too large");
        finish();
        return;
    }

    countedRequest = true;
    auto head = parseHead(std::string_view(buffer).substr(0, headEnd + 2));
    if (!head) {
        reject(kHttpStatusBadRequest, "bad_request", "malformed request headers");
        finish();
        return;
    }

    Request req;
    req.remote = remote;
    {
        const auto& line = head->firstLine;
        const auto sp1 = line.find(' ');
        const auto sp2 = line.rfind(' ');
        if (sp1 == std::string::npos || sp2 == sp1 || line.compare(sp2 + 1, 5, "HTTP/") != 0) {
            reject(kHttpStatusBadRequest, "bad_request", "malformed request line");
            finish();
            return;
        }
        req.method = line.substr(0, sp1);
        req.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
        const auto version = line.substr(sp2 + 1);
        if (version != "HTTP/1.1" && version != "HTTP/1.0") {
            reject(kHttpStatusBadRequest, "bad_request", "unsupported HTTP version");
            finish();
            return;
        }
    }
    if (req.method.empty() || !std::ranges::all_of(req.method, [](char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; }) ||
        req.target.empty() || req.target.front() != '/') {
        reject(kHttpStatusBadRequest, "bad_request", "malformed request line");
        finish();
        return;
    }
    req.headers = std::move(head->headers);

    const auto qpos = req.target.find('?');
    req.path = urlDecode(std::string_view(req.target).substr(0, qpos));
    if (qpos != std::string::npos) req.query = parseQuery(std::string_view(req.target).substr(qpos + 1));
    if (req.path.find('\0') != std::string::npos) {
        reject(kHttpStatusBadRequest, "bad_request", "invalid path");
        finish();
        return;
    }

    std::string rid = req.header("X-Request-Id");
    if (!validRequestId(rid)) rid = std::format("{}-{}", idPrefix(), requestCounter_.fetch_add(1) + 1);
    req.requestId = rid;

    std::string body = buffer.substr(headEnd + 4);
    const auto te = toLower(req.header("Transfer-Encoding"));
    const auto cl = req.header("Content-Length");
    if (!te.empty() && !cl.empty()) {
        reject(kHttpStatusBadRequest, "bad_request", "both Transfer-Encoding and Content-Length present");
        finish();
        return;
    }
    if (toLower(req.header("Expect")) == "100-continue") {
        sendAll(fd, "HTTP/1.1 100 Continue\r\n\r\n", options_.writeTimeout);
    }

    if (!te.empty()) {
        if (te != "chunked") {
            reject(kHttpStatusNotImplemented, "not_implemented", "unsupported transfer encoding");
            finish();
            return;
        }
        std::string decoded;
        std::string pending = std::move(body);
        bool done = false;
        while (!done) {
            std::size_t pos = 0;
            while (true) {
                const auto eol = pending.find("\r\n", pos);
                if (eol == std::string::npos) break;
                std::size_t size = 0;
                const auto sizeText = std::string_view(pending).substr(pos, eol - pos);
                const auto semi = sizeText.find(';');
                const auto hex = sizeText.substr(0, semi);
                auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), size, 16);
                if (ec != std::errc() || ptr != hex.data() + hex.size()) {
                    reject(kHttpStatusBadRequest, "bad_request", "invalid chunk size");
                    finish();
                    return;
                }
                if (size == 0) {
                    done = true;
                    break;
                }
                if (decoded.size() + size > options_.maxBodyBytes) {
                    reject(kHttpStatusPayloadTooLarge, "payload_too_large", "request body too large");
                    finish();
                    return;
                }
                if (pending.size() < eol + 2 + size + 2) break;
                decoded.append(pending, eol + 2, size);
                pos = eol + 2 + size + 2;
            }
            pending.erase(0, pos);
            if (done) break;
            const auto rr = readSome(fd, chunk.data(), chunk.size(), options_.readTimeout);
            if (rr.kind != ReadResult::Data) {
                reject(rr.kind == ReadResult::Timeout ? kHttpStatusRequestTimeout : kHttpStatusBadRequest, "bad_request",
                       "incomplete chunked body");
                finish();
                return;
            }
            bytesIn += rr.size;
            pending.append(chunk.data(), rr.size);
        }
        body = std::move(decoded);
    } else if (!cl.empty()) {
        std::size_t want = 0;
        auto [ptr, ec] = std::from_chars(cl.data(), cl.data() + cl.size(), want);
        if (ec != std::errc() || ptr != cl.data() + cl.size()) {
            reject(kHttpStatusBadRequest, "bad_request", "invalid Content-Length");
            finish();
            return;
        }
        if (want > options_.maxBodyBytes) {
            reject(kHttpStatusPayloadTooLarge, "payload_too_large", "request body too large");
            finish();
            return;
        }
        while (body.size() < want) {
            const auto rr = readSome(fd, chunk.data(), std::min(chunk.size(), want - body.size()), options_.readTimeout);
            if (rr.kind != ReadResult::Data) {
                reject(rr.kind == ReadResult::Timeout ? kHttpStatusRequestTimeout : kHttpStatusBadRequest, "bad_request",
                       "incomplete request body");
                finish();
                return;
            }
            bytesIn += rr.size;
            body.append(chunk.data(), rr.size);
        }
        body.resize(want);
    } else {
        body.clear();
    }
    req.body = std::move(body);

    Response res = dispatch(req);
    if (!options_.corsOrigin.empty()) {
        res.headers["Access-Control-Allow-Origin"] = options_.corsOrigin;
        res.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
        res.headers["Access-Control-Allow-Headers"] = "Authorization, Content-Type, X-Request-Id";
        res.headers["Vary"] = "Origin";
    }
    res.headers["X-Request-Id"] = req.requestId;
    res.headers["Server"] = options_.serverName;
    res.headers["Connection"] = "close";
    res.headers["X-Content-Type-Options"] = "nosniff";

    const bool headOnly = req.method == "HEAD";
    const bool noBody = res.status == 204 || res.status == 304 || (res.status >= 100 && res.status < 200);
    finalStatus = res.status;

    if (res.stream && !headOnly) {
        res.headers["Transfer-Encoding"] = "chunked";
        SocketWriter writer(fd, options_.writeTimeout, bytesOut, res.status, res.headers);
        try {
            res.stream(writer);
        } catch (const std::exception& e) {
            log::warn("http", "stream producer for {} failed: {}", req.path, e.what());
        }
        writer.finish();
        res.status = writer.status();
        finalStatus = res.status;
    } else {
        if (!noBody) res.headers["Content-Length"] = std::to_string(res.stream ? 0 : res.body.size());
        std::string wire = std::format("HTTP/1.1 {} {}\r\n", res.status, statusText(res.status));
        for (const auto& [k, v] : res.headers) appendHeaderLine(wire, k, v);
        wire += "\r\n";
        bool ok = sendAll(fd, wire, options_.writeTimeout);
        bytesOut += wire.size();
        if (ok && !headOnly && !noBody && !res.stream) {
            sendAll(fd, res.body, options_.writeTimeout);
            bytesOut += res.body.size();
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    log::debug("http", "{} {} {} {}ms rid={} from {}", req.method, req.path, res.status, elapsed.count(), req.requestId, remote);
    if (res.status >= kHttpServerErrorStatusClassStart)
        log::warn("http", "{} {} answered {} rid={}", req.method, req.path, res.status, req.requestId);
    finish();
}

std::optional<Url> Url::parse(std::string_view text, std::string* error) {
    auto fail = [&](const char* message) -> std::optional<Url> {
        if (error != nullptr) *error = message;
        return std::nullopt;
    };
    Url url;
    const auto scheme = text.find("://");
    if (scheme == std::string_view::npos) return fail("missing scheme");
    url.scheme = toLower(text.substr(0, scheme));
    if (url.scheme != "http" && url.scheme != "https") return fail("unsupported scheme");
    url.port = url.scheme == "https" ? kDefaultHttpsPort : kDefaultHttpPort;
    text.remove_prefix(scheme + 3);
    const auto slash = text.find_first_of("/?");
    auto authority = text.substr(0, slash);
    url.target = slash == std::string_view::npos ? "/" : std::string(text.substr(slash));
    if (url.target.front() == '?') url.target.insert(url.target.begin(), '/');
    if (authority.empty()) return fail("missing host");
    if (authority.find('@') != std::string_view::npos) return fail("credentials in URL are not supported");
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return fail("bad IPv6 literal");
        url.host = std::string(authority.substr(1, close - 1));
        authority.remove_prefix(close + 1);
        if (!authority.empty()) {
            if (authority.front() != ':') return fail("bad authority");
            authority.remove_prefix(1);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            url.host = std::string(authority);
            authority = {};
        } else {
            url.host = std::string(authority.substr(0, colon));
            authority.remove_prefix(colon + 1);
        }
    }
    if (!authority.empty()) {
        unsigned value = 0;
        auto [ptr, ec] = std::from_chars(authority.data(), authority.data() + authority.size(), value);
        if (ec != std::errc() || ptr != authority.data() + authority.size() || value == 0 || value > kMaximumPortNumber)
            return fail("bad port");
        url.port = static_cast<std::uint16_t>(value);
    }
    if (url.host.empty()) return fail("missing host");
    return url;
}

std::string Url::authority() const {
    const bool v6 = host.find(':') != std::string::npos;
    return std::format("{}{}{}:{}", v6 ? "[" : "", host, v6 ? "]" : "", port);
}

namespace {

int connectTo(const Url& url, std::chrono::milliseconds timeout, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* res = nullptr;
    const std::string port = std::to_string(url.port);
    if (const int rc = ::getaddrinfo(url.host.c_str(), port.c_str(), &hints, &res); rc != 0) {
        error = std::format("cannot resolve {}: {}", url.host, ::gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    error = "connection failed";
    for (addrinfo* ai = res; ai != nullptr && fd < 0; ai = ai->ai_next) {
        int s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        setNonBlocking(s);
        suppressSigpipe(s);
        int rc = ::connect(s, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            if (waitFor(s, POLLOUT, timeout) == WaitResult::Ready) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                ::getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len);
                if (soerr == 0)
                    rc = 0;
                else
                    errno = soerr;
            } else {
                errno = ETIMEDOUT;
            }
        }
        if (rc == 0) {
            const int on = 1;
            ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            fd = s;
        } else {
            error = std::format("cannot connect to {}: {}", url.authority(), std::strerror(errno));
            ::close(s);
        }
    }
    ::freeaddrinfo(res);
    if (fd >= 0) error.clear();
    return fd;
}

}

ClientResponse httpStream(const Url& url, std::string_view method, std::string_view body, const Headers& headers,
                          const ClientOptions& options, const HeaderCallback& onHeaders, const BodyCallback& onBody) {
    ClientResponse out;
    if (url.scheme != "http") {
        out.error = "https is not supported by the built-in client; point Gygax at the plain-HTTP port or a local TLS terminator";
        return out;
    }
    int fd = connectTo(url, options.connectTimeout, out.error);
    if (fd < 0) return out;

    Headers sendHeaders = headers;
    sendHeaders.try_emplace("Host", url.authority());
    sendHeaders.try_emplace("User-Agent", "gygax");
    sendHeaders.try_emplace("Accept", "*/*");
    sendHeaders["Connection"] = "close";
    if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
        sendHeaders["Content-Length"] = std::to_string(body.size());
    }
    std::string wire = std::format("{} {} HTTP/1.1\r\n", method, url.target);
    for (const auto& [k, v] : sendHeaders) appendHeaderLine(wire, k, v);
    wire += "\r\n";
    wire.append(body);
    const bool sent = sendAll(fd, wire, options.readTimeout);

    std::string buffer;
    std::array<char, 16384> chunk{};
    std::size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos) {
        if (buffer.size() > kMaximumResponseHeaderBufferBytes) {
            out.error = "response headers too large";
            closeFd(fd);
            return out;
        }
        const auto rr = readSome(fd, chunk.data(), chunk.size(), sent ? options.readTimeout : kInitialResponseReadTimeout);
        if (rr.kind != ReadResult::Data) {
            if (!sent)
                out.error = "failed to send request";
            else
                out.error = rr.kind == ReadResult::Timeout ? "timed out waiting for response" : "connection closed before response";
            closeFd(fd);
            return out;
        }
        buffer.append(chunk.data(), rr.size);
        headEnd = buffer.find("\r\n\r\n");
    }
    auto head = parseHead(std::string_view(buffer).substr(0, headEnd + 2));
    if (!head || head->firstLine.compare(0, 5, "HTTP/") != 0) {
        out.error = "malformed response";
        closeFd(fd);
        return out;
    }
    const auto sp = head->firstLine.find(' ');
    int status = 0;
    if (sp != std::string::npos) {
        const auto code = std::string_view(head->firstLine).substr(sp + 1, 3);
        std::from_chars(code.data(), code.data() + code.size(), status);
    }
    if (status < kMinimumHttpStatusCode || status > kMaximumHttpStatusCode) {
        out.error = "malformed status line";
        closeFd(fd);
        return out;
    }
    out.status = status;
    out.headers = std::move(head->headers);
    if (onHeaders && !onHeaders(out.status, out.headers)) {
        out.ok = true;
        closeFd(fd);
        return out;
    }

    std::string pending = buffer.substr(headEnd + 4);
    const bool noBody = method == "HEAD" || status == 204 || status == 304 || (status >= 100 && status < 200);
    const auto te = toLower(out.headers.contains("Transfer-Encoding") ? out.headers.at("Transfer-Encoding") : "");
    std::size_t total = 0;
    bool keepGoing = true;
    auto emit = [&](std::string_view data) {
        if (data.empty() || !keepGoing) return;
        total += data.size();
        if (total > options.maxResponseBytes) {
            out.error = "response exceeds size limit";
            keepGoing = false;
            return;
        }
        if (onBody) {
            keepGoing = onBody(data);
        } else {
            out.body.append(data);
        }
    };

    if (noBody) {
        out.ok = true;
    } else if (te.find("chunked") != std::string::npos) {
        bool finished = false;
        while (!finished && keepGoing) {
            while (true) {
                const auto eol = pending.find("\r\n");
                if (eol == std::string::npos) break;
                std::size_t size = 0;
                const auto sizeText = std::string_view(pending).substr(0, eol);
                const auto hex = sizeText.substr(0, sizeText.find(';'));
                auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), size, 16);
                if (ec != std::errc() || ptr != hex.data() + hex.size()) {
                    out.error = "invalid chunk in response";
                    keepGoing = false;
                    break;
                }
                if (size == 0) {
                    finished = true;
                    break;
                }
                if (pending.size() < eol + 2 + size + 2) break;
                emit(std::string_view(pending).substr(eol + 2, size));
                pending.erase(0, eol + 2 + size + 2);
                if (!keepGoing) break;
            }
            if (finished || !keepGoing) break;
            const auto rr = readSome(fd, chunk.data(), chunk.size(), options.readTimeout);
            if (rr.kind != ReadResult::Data) {
                out.error = rr.kind == ReadResult::Timeout ? "timed out reading response body" : "connection closed mid-stream";
                keepGoing = false;
                break;
            }
            pending.append(chunk.data(), rr.size);
        }
        out.ok = out.error.empty();
    } else if (out.headers.contains("Content-Length")) {
        std::size_t want = 0;
        const auto& cl = out.headers.at("Content-Length");
        std::from_chars(cl.data(), cl.data() + cl.size(), want);
        std::size_t got = std::min(want, pending.size());
        emit(std::string_view(pending).substr(0, got));
        while (got < want && keepGoing) {
            const auto rr = readSome(fd, chunk.data(), std::min(chunk.size(), want - got), options.readTimeout);
            if (rr.kind != ReadResult::Data) {
                out.error = rr.kind == ReadResult::Timeout ? "timed out reading response body" : "connection closed mid-body";
                keepGoing = false;
                break;
            }
            got += rr.size;
            emit(std::string_view(chunk.data(), rr.size));
        }
        out.ok = out.error.empty();
    } else {
        emit(pending);
        while (keepGoing) {
            const auto rr = readSome(fd, chunk.data(), chunk.size(), options.readTimeout);
            if (rr.kind == ReadResult::Eof) break;
            if (rr.kind != ReadResult::Data) {
                out.error = "timed out reading response body";
                keepGoing = false;
                break;
            }
            emit(std::string_view(chunk.data(), rr.size));
        }
        out.ok = out.error.empty();
    }
    if (!keepGoing && out.error.empty()) out.ok = true;
    closeFd(fd);
    return out;
}

ClientResponse httpRequest(const Url& url, std::string_view method, std::string_view body, const Headers& headers,
                           const ClientOptions& options) {
    return httpStream(url, method, body, headers, options, nullptr, nullptr);
}

}
