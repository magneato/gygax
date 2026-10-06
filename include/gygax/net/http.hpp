#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace gygax::net {

inline constexpr int kHttpStatusContinue = 100;
inline constexpr int kHttpStatusSwitchingProtocols = 101;
inline constexpr int kHttpStatusOk = 200;
inline constexpr int kHttpStatusCreated = 201;
inline constexpr int kHttpStatusAccepted = 202;
inline constexpr int kHttpStatusNoContent = 204;
inline constexpr int kHttpStatusMovedPermanently = 301;
inline constexpr int kHttpStatusFound = 302;
inline constexpr int kHttpStatusNotModified = 304;
inline constexpr int kHttpStatusBadRequest = 400;
inline constexpr int kHttpStatusUnauthorized = 401;
inline constexpr int kHttpStatusForbidden = 403;
inline constexpr int kHttpStatusNotFound = 404;
inline constexpr int kHttpStatusMethodNotAllowed = 405;
inline constexpr int kHttpStatusRequestTimeout = 408;
inline constexpr int kHttpStatusConflict = 409;
inline constexpr int kHttpStatusLengthRequired = 411;
inline constexpr int kHttpStatusPayloadTooLarge = 413;
inline constexpr int kHttpStatusUnsupportedMediaType = 415;
inline constexpr int kHttpStatusUnprocessableEntity = 422;
inline constexpr int kHttpStatusTooManyRequests = 429;
inline constexpr int kHttpStatusRequestHeaderFieldsTooLarge = 431;
inline constexpr int kHttpStatusInternalServerError = 500;
inline constexpr int kHttpStatusNotImplemented = 501;
inline constexpr int kHttpStatusBadGateway = 502;
inline constexpr int kHttpStatusServiceUnavailable = 503;
inline constexpr int kHttpStatusGatewayTimeout = 504;

inline constexpr const char* kDefaultHttpListenAddress = "127.0.0.1";
inline constexpr std::size_t kDefaultHttpWorkerCount = 8;
inline constexpr std::size_t kDefaultHttpQueueCapacity = 256;
inline constexpr std::size_t kDefaultHttpHeaderLimitBytes = 16 * 1024;
inline constexpr std::size_t kDefaultHttpBodyLimitBytes = 8 * 1024 * 1024;
inline constexpr std::chrono::milliseconds kDefaultHttpReadTimeout{10000};
inline constexpr std::chrono::milliseconds kDefaultHttpWriteTimeout{30000};
inline constexpr std::uint16_t kDefaultHttpPort = 80;
inline constexpr std::uint16_t kDefaultHttpsPort = 443;
inline constexpr std::chrono::milliseconds kDefaultHttpConnectTimeout{3000};
inline constexpr std::chrono::milliseconds kDefaultHttpClientReadTimeout{60000};
inline constexpr std::size_t kDefaultHttpResponseLimitBytes = 64ULL * 1024 * 1024;

struct CaseInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const;
};

using Headers = std::map<std::string, std::string, CaseInsensitiveLess>;
using Query = std::map<std::string, std::string>;

struct Request {
    std::string method;
    std::string target;
    std::string path;
    Query query;
    Headers headers;
    std::string body;
    std::string remote;
    std::string requestId;
    std::map<std::string, std::string> params;

    [[nodiscard]] std::string header(const std::string& name, const std::string& fallback = {}) const;
    [[nodiscard]] std::string queryValue(const std::string& name, const std::string& fallback = {}) const;
};

class ChunkWriter {
public:
    virtual ~ChunkWriter() = default;
    virtual bool write(std::string_view data) = 0;
    virtual void setStatus(int status) = 0;
    virtual void setHeader(const std::string& name, const std::string& value) = 0;
    [[nodiscard]] virtual bool headersSent() const = 0;
    [[nodiscard]] virtual bool aborted() const = 0;
};

struct Response {
    int status = kHttpStatusOk;
    Headers headers;
    std::string body;
    std::function<void(ChunkWriter&)> stream;

    static Response json(int status, std::string body);
    static Response text(int status, std::string body, std::string contentType = "text/plain; charset=utf-8");
    static Response error(int status, std::string_view code, std::string_view message);
    static Response streaming(int status, std::string contentType, std::function<void(ChunkWriter&)> producer);
};

using Handler = std::function<Response(Request&)>;
using Interceptor = std::function<std::optional<Response>(Request&)>;

struct ServerOptions {
    std::string host = kDefaultHttpListenAddress;
    std::uint16_t port = 0;
    std::size_t workers = kDefaultHttpWorkerCount;
    std::size_t maxQueuedConnections = kDefaultHttpQueueCapacity;
    std::size_t maxHeaderBytes = kDefaultHttpHeaderLimitBytes;
    std::size_t maxBodyBytes = kDefaultHttpBodyLimitBytes;
    std::chrono::milliseconds readTimeout = kDefaultHttpReadTimeout;
    std::chrono::milliseconds writeTimeout = kDefaultHttpWriteTimeout;
    std::string corsOrigin;
    std::string serverName = "gygax";
};

struct ServerStats {
    std::uint64_t accepted = 0;
    std::uint64_t rejectedOverload = 0;
    std::uint64_t requests = 0;
    std::uint64_t badRequests = 0;
    std::uint64_t responses2xx = 0;
    std::uint64_t responses3xx = 0;
    std::uint64_t responses4xx = 0;
    std::uint64_t responses5xx = 0;
    std::uint64_t inFlight = 0;
    std::uint64_t bytesIn = 0;
    std::uint64_t bytesOut = 0;
};

class Server {
public:
    explicit Server(ServerOptions options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void route(std::string method, const std::string& pattern, Handler handler);
    void intercept(Interceptor interceptor);

    [[nodiscard]] bool start(std::string* error = nullptr);
    void stop();
    [[nodiscard]] bool running() const { return running_.load(); }
    [[nodiscard]] std::uint16_t port() const { return boundPort_; }
    [[nodiscard]] const ServerOptions& options() const { return options_; }
    [[nodiscard]] ServerStats stats() const;

private:
    struct Route {
        std::string method;
        std::vector<std::string> segments;
        bool wildcard = false;
        Handler handler;
    };

    void acceptLoop();
    void workerLoop();
    void serve(int fd, const std::string& remote);
    Response dispatch(Request& request);
    bool matchRoute(const Route& route, const std::vector<std::string>& parts, Request& request) const;

    ServerOptions options_;
    std::vector<Route> routes_;
    std::vector<Interceptor> interceptors_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> requestCounter_{0};
    int listenFd_ = -1;
    int wakeRead_ = -1;
    int wakeWrite_ = -1;
    std::uint16_t boundPort_ = 0;
    std::thread acceptor_;
    std::vector<std::thread> workers_;
    std::mutex queueMutex_;
    std::condition_variable queueReady_;
    struct Pending {
        int fd;
        std::string remote;
    };
    std::deque<Pending> queue_;

    mutable std::mutex statsMutex_;
    ServerStats stats_;
};

struct Url {
    std::string scheme = "http";
    std::string host;
    std::uint16_t port = kDefaultHttpPort;
    std::string target = "/";

    static std::optional<Url> parse(std::string_view text, std::string* error = nullptr);
    [[nodiscard]] std::string authority() const;
};

struct ClientOptions {
    std::chrono::milliseconds connectTimeout = kDefaultHttpConnectTimeout;
    std::chrono::milliseconds readTimeout = kDefaultHttpClientReadTimeout;
    std::size_t maxResponseBytes = kDefaultHttpResponseLimitBytes;
};

struct ClientResponse {
    bool ok = false;
    int status = 0;
    Headers headers;
    std::string body;
    std::string error;
};

using HeaderCallback = std::function<bool(int status, const Headers& headers)>;
using BodyCallback = std::function<bool(std::string_view data)>;

ClientResponse httpRequest(const Url& url, std::string_view method, std::string_view body = {}, const Headers& headers = {},
                           const ClientOptions& options = {});

ClientResponse httpStream(const Url& url, std::string_view method, std::string_view body, const Headers& headers,
                          const ClientOptions& options, const HeaderCallback& onHeaders, const BodyCallback& onBody);

std::string urlDecode(std::string_view text);
std::string urlEncode(std::string_view text);
std::string statusText(int status);
bool constantTimeEquals(std::string_view a, std::string_view b);

}
