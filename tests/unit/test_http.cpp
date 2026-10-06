#include <gtest/gtest.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include <gygax/core/log.hpp>
#include <gygax/net/http.hpp>

#include "support.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

using namespace gygax;
using namespace gygax::net;

namespace {

constexpr std::size_t kHttpTestMaxBodyBytes = 64 * 1024;
constexpr std::size_t kHttpTestMaxHeaderBytes = 4096;
constexpr auto kHttpTestReadTimeout = std::chrono::milliseconds(300);

class HttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        log::setLevel(log::Level::Error);
        ServerOptions o;
        o.port = 0;
        o.workers = 4;
        o.maxBodyBytes = kHttpTestMaxBodyBytes;
        o.maxHeaderBytes = kHttpTestMaxHeaderBytes;
        o.readTimeout = kHttpTestReadTimeout;
        server = std::make_unique<Server>(o);
        server->route("GET", "/hello/:name", [](Request& r) { return Response::json(200, "{\"hi\":\"" + r.params["name"] + "\"}"); });
        server->route("POST", "/echo", [](Request& r) { return Response::text(200, r.body); });
        server->route("GET", "/query", [](Request& r) { return Response::text(200, r.queryValue("a") + "|" + r.queryValue("b")); });
        server->route("GET", "/files/*", [](Request& r) { return Response::text(200, r.path); });
        server->route("GET", "/boom", [](Request&) -> Response { throw std::runtime_error("boom"); });
        server->route("GET", "/stream", [](Request&) {
            return Response::streaming(200, "text/plain", [](ChunkWriter& w) {
                for (int i = 0; i < 3; ++i) w.write("chunk" + std::to_string(i) + ";");
            });
        });
        server->route("GET", "/late-status", [](Request&) {
            return Response::streaming(200, "text/plain", [](ChunkWriter& w) {
                w.setStatus(418);
                w.setHeader("X-Reason", "teapot");
                w.write("short and stout");
            });
        });
        server->route("GET", "/rid", [](Request& r) { return Response::text(200, r.requestId); });
        server->intercept([](Request& r) -> std::optional<Response> {
            if (r.path == "/blocked") return Response::error(403, "blocked", "nope");
            return std::nullopt;
        });
        std::string err;
        ASSERT_TRUE(server->start(&err)) << err;
    }

    void TearDown() override { server->stop(); }

    std::string raw(const std::string& request) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(server->port());
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
        std::string out;
        char buf[2048];
        while (true) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.append(buf, static_cast<std::size_t>(n));
        }
        ::close(fd);
        return out;
    }

    std::unique_ptr<Server> server;
};

}

TEST_F(HttpTest, RoutesWithPathParametersAndDecoding) {
    auto r = support::get(*server, "/hello/w%6Frld");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, R"({"hi":"world"})");
    EXPECT_EQ(r.headers.at("content-type"), "application/json");
}

TEST_F(HttpTest, ParsesQueryStrings) {
    auto r = support::get(*server, "/query?a=1%202&b=x+y");
    EXPECT_EQ(r.body, "1 2|x y");
}

TEST_F(HttpTest, WildcardRoutes) {
    EXPECT_EQ(support::get(*server, "/files/a/b/c.txt").body, "/files/a/b/c.txt");
}

TEST_F(HttpTest, EchoesLargeBodies) {
    const std::string body(60 * 1024, 'q');
    auto r = support::post(*server, "/echo", body);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, body);
}

TEST_F(HttpTest, RejectsOversizedBodiesWith413) {
    auto r = support::post(*server, "/echo", std::string(200 * 1024, 'q'));
    EXPECT_EQ(r.status, 413);
}

TEST_F(HttpTest, NotFoundAndMethodNotAllowed) {
    auto missing = support::get(*server, "/nope");
    EXPECT_EQ(missing.status, 404);
    auto parsed = json::parse(missing.body);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->find("error")->getString("code"), "not_found");
    auto wrong = support::get(*server, "/echo");
    EXPECT_EQ(wrong.status, 405);
    EXPECT_EQ(wrong.headers.at("Allow"), "POST");
}

TEST_F(HttpTest, HandlerExceptionsBecome500WithoutLeakingDetails) {
    auto r = support::get(*server, "/boom");
    EXPECT_EQ(r.status, 500);
    EXPECT_EQ(r.body.find("boom"), std::string::npos);
    EXPECT_EQ(support::get(*server, "/hello/x").status, 200);
}

TEST_F(HttpTest, InterceptorsShortCircuit) {
    EXPECT_EQ(support::get(*server, "/blocked").status, 403);
}

TEST_F(HttpTest, ChunkedStreaming) {
    auto r = support::get(*server, "/stream");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "chunk0;chunk1;chunk2;");
}

TEST_F(HttpTest, StreamStatusCanChangeBeforeFirstWrite) {
    auto r = support::get(*server, "/late-status");
    EXPECT_EQ(r.status, 418);
    EXPECT_EQ(r.headers.at("X-Reason"), "teapot");
    EXPECT_EQ(r.body, "short and stout");
}

TEST_F(HttpTest, EchoesValidRequestIdsAndReplacesInvalidOnes) {
    EXPECT_EQ(support::get(*server, "/rid", {{"X-Request-Id", "abc-123"}}).body, "abc-123");
    const auto generated = support::get(*server, "/rid", {{"X-Request-Id", "bad id with spaces"}}).body;
    EXPECT_NE(generated, "bad id with spaces");
    EXPECT_FALSE(generated.empty());
}

TEST_F(HttpTest, MalformedRequestsGet400) {
    EXPECT_NE(raw("GARBAGE\r\n\r\n").find("400"), std::string::npos);
    EXPECT_NE(raw("GET /x HTTP/1.1\r\nBad Header: v\r\n\r\n").find("400"), std::string::npos);
    EXPECT_NE(raw("POST /echo HTTP/1.1\r\nContent-Length: abc\r\n\r\n").find("400"), std::string::npos);
    EXPECT_NE(raw("POST /echo HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n").find("400"), std::string::npos);
    EXPECT_NE(raw("GET /x HTTP/2.0\r\n\r\n").find("400"), std::string::npos);
}

TEST_F(HttpTest, OversizedHeadersGet431) {
    const std::string big(8000, 'a');
    EXPECT_NE(raw("GET /x HTTP/1.1\r\nX-Big: " + big + "\r\n\r\n").find("431"), std::string::npos);
}

TEST_F(HttpTest, DecodesChunkedRequestBodies) {
    const auto reply = raw("POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    EXPECT_NE(reply.find("200 OK"), std::string::npos);
    EXPECT_NE(reply.find("hello world"), std::string::npos);
}

TEST_F(HttpTest, SlowClientsTimeOutWith408) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server->port());
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    const std::string partial = "GET /x HTTP/1.1\r\nHost: a";
    ::send(fd, partial.data(), partial.size(), MSG_NOSIGNAL);
    std::string out;
    char buf[512];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n > 0) out.assign(buf, static_cast<std::size_t>(n));
    ::close(fd);
    EXPECT_NE(out.find("408"), std::string::npos);
}

TEST_F(HttpTest, HeadReturnsNoBody) {
    const auto reply = raw("HEAD /hello/x HTTP/1.1\r\n\r\n");
    EXPECT_NE(reply.find("200 OK"), std::string::npos);
    EXPECT_EQ(reply.find("{\"hi\""), std::string::npos);
}

TEST_F(HttpTest, StatsCountRequestsAndResponses) {
    (void)support::get(*server, "/hello/a");
    (void)support::get(*server, "/missing");
    ASSERT_TRUE(support::waitUntil([&] { return server->stats().requests >= 2; }));
    const auto st = server->stats();
    EXPECT_GE(st.responses2xx, 1U);
    EXPECT_GE(st.responses4xx, 1U);
    EXPECT_GT(st.bytesOut, 0U);
}

TEST_F(HttpTest, ConcurrentClientsAreServed) {
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    for (int i = 0; i < 24; ++i) {
        threads.emplace_back([&, i] {
            auto r = support::post(*server, "/echo", "payload" + std::to_string(i));
            if (r.status == 200 && r.body == "payload" + std::to_string(i)) ++ok;
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(ok.load(), 24);
}

TEST(HttpUtil, UrlParsing) {
    auto u = Url::parse("http://example.com:8080/a/b?x=1");
    ASSERT_TRUE(u);
    EXPECT_EQ(u->host, "example.com");
    EXPECT_EQ(u->port, 8080);
    EXPECT_EQ(u->target, "/a/b?x=1");
    EXPECT_EQ(Url::parse("http://[::1]:9/")->host, "::1");
    EXPECT_EQ(Url::parse("https://h")->port, 443);
    std::string err;
    EXPECT_FALSE(Url::parse("ftp://h", &err));
    EXPECT_FALSE(Url::parse("http://", &err));
    EXPECT_FALSE(Url::parse("http://h:99999/", &err));
    EXPECT_FALSE(Url::parse("http://user:pw@h/", &err));
}

TEST(HttpUtil, EncodeDecodeRoundTrip) {
    const std::string raw = "a b&c=d/é";
    EXPECT_EQ(urlDecode(urlEncode(raw)), raw);
    EXPECT_EQ(urlEncode("a b"), "a%20b");
}

TEST(HttpUtil, ConstantTimeEquals) {
    EXPECT_TRUE(constantTimeEquals("secret", "secret"));
    EXPECT_FALSE(constantTimeEquals("secret", "secreT"));
    EXPECT_FALSE(constantTimeEquals("secret", "secret2"));
    EXPECT_TRUE(constantTimeEquals("", ""));
}

TEST(HttpClient, ReportsConnectionRefused) {
    auto r = httpRequest(*Url::parse("http://127.0.0.1:1/"), "GET");
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(r.status, 0);
}

TEST(HttpClient, HttpsIsRejectedExplicitly) {
    auto r = httpRequest(*Url::parse("https://127.0.0.1:1/"), "GET");
    EXPECT_NE(r.error.find("https"), std::string::npos);
}

TEST(HttpServer, RefusesToBindToAnOccupiedPort) {
    ServerOptions o;
    o.port = 0;
    Server first(o);
    ASSERT_TRUE(first.start());
    ServerOptions second = o;
    second.port = first.port();
    Server dup(second);
    std::string err;
    EXPECT_FALSE(dup.start(&err));
    EXPECT_FALSE(err.empty());
}

TEST(HttpServer, RejectsWhenOverloaded) {
    ServerOptions o;
    o.port = 0;
    o.workers = 1;
    o.maxQueuedConnections = 1;
    o.readTimeout = std::chrono::milliseconds(1500);
    Server s(o);
    s.route("GET", "/x", [](Request&) { return Response::text(200, "ok"); });
    ASSERT_TRUE(s.start());
    std::vector<int> fds;
    for (int i = 0; i < 6; ++i) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(s.port());
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        fds.push_back(fd);
    }
    ASSERT_TRUE(support::waitUntil([&] { return s.stats().rejectedOverload > 0; }));
    for (const int fd : fds) ::close(fd);
}
