#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

#include <gygax/core/log.hpp>
#include <gygax/inference/backend.hpp>

#include "support.hpp"

using namespace gygax;
using namespace gygax::inference;

namespace {

constexpr auto kBackendProbeInterval = std::chrono::milliseconds(20);

ChatRequest userMessage(const std::string& text, const std::string& model = "") {
    ChatRequest r;
    r.model = model;
    r.messages.push_back({"user", text});
    return r;
}

class ScriptedBackend final : public Backend {
public:
    ScriptedBackend(std::string name, bool failChat, std::vector<std::string> models, bool remote = false)
        : name_(std::move(name)), fail_(failChat), models_(std::move(models)), remote_(remote) {}

    [[nodiscard]] std::string kind() const override { return "scripted"; }
    [[nodiscard]] std::string endpoint() const override { return name_; }
    [[nodiscard]] bool isRemote() const override { return remote_; }

    ChatResult chat(const ChatRequest& request) override {
        ++calls;
        ChatResult r;
        if (fail_) {
            r.status = 503;
            r.retryable = true;
            r.error = name_ + " unavailable";
            return r;
        }
        r.ok = true;
        r.status = 200;
        r.text = name_ + ":" + request.messages.back().content;
        r.latencyMs = latency;
        return r;
    }

    bool listModels(std::vector<std::string>& out, std::string& error) override {
        if (probeFails) {
            error = "probe failed";
            return false;
        }
        out = models_;
        return true;
    }

    std::atomic<int> calls{0};
    std::atomic<bool> probeFails{false};
    std::int64_t latency = 10;

private:
    std::string name_;
    bool fail_;
    std::vector<std::string> models_;
    bool remote_;
};

}

TEST(Echo, PlainAndProtocolModes) {
    EchoBackend echo;
    auto plain = echo.chat(userMessage("hi"));
    ASSERT_TRUE(plain.ok);
    EXPECT_EQ(plain.text, "echo: hi");

    ChatRequest agent;
    agent.messages.push_back({"system", std::string(EchoBackend::kProtocolMarker)});
    agent.messages.push_back({"user", "!tool math.eval 1+1"});
    auto call = json::parse(echo.chat(agent).text);
    ASSERT_TRUE(call);
    EXPECT_EQ(call->getString("tool"), "math.eval");
    EXPECT_EQ(call->getString("input"), "1+1");

    agent.messages.push_back({"assistant", "x"});
    agent.messages.push_back({"user", "Tool result (math.eval): 2"});
    auto answer = json::parse(echo.chat(agent).text);
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->getString("answer"), "2");

    ChatRequest empty;
    EXPECT_FALSE(echo.chat(empty).ok);
}

TEST(NormalizeModel, LowercasesAndDropsLatest) {
    EXPECT_EQ(normalizeModel("Llama3:Latest"), "llama3");
    EXPECT_EQ(normalizeModel("llama3:8b"), "llama3:8b");
    EXPECT_EQ(normalizeModel(":latest"), ":latest");
}

TEST(MakeBackend, ParsesSpecs) {
    std::string err;
    EXPECT_TRUE(makeBackend("echo", &err));
    auto b = makeBackend("http://127.0.0.1:11434/v1;model=llama3;key=abc", &err);
    ASSERT_TRUE(b) << err;
    EXPECT_EQ(b->kind(), "chat-api");
    EXPECT_FALSE(b->isRemote());
    auto peer = makeBackend("http://10.0.0.5:1984/v1;peer=1", &err);
    ASSERT_TRUE(peer);
    EXPECT_TRUE(peer->isRemote());
    EXPECT_FALSE(makeBackend("not a url", &err));
    EXPECT_FALSE(makeBackend("http://h/v1;bogus=1", &err));
    EXPECT_NE(err.find("bogus"), std::string::npos);
}

TEST(EnginePool, RoutesByModelInventory) {
    EnginePool pool;
    auto a = std::make_shared<ScriptedBackend>("a", false, std::vector<std::string>{"alpha"});
    auto b = std::make_shared<ScriptedBackend>("b", false, std::vector<std::string>{"beta:latest"});
    ASSERT_TRUE(pool.addEngine("a", a));
    ASSERT_TRUE(pool.addEngine("b", b));
    pool.probeNow();
    auto ra = pool.chat(userMessage("x", "alpha"));
    auto rb = pool.chat(userMessage("y", "BETA"));
    ASSERT_TRUE(ra.ok);
    ASSERT_TRUE(rb.ok);
    EXPECT_EQ(ra.text, "a:x");
    EXPECT_EQ(rb.text, "b:y");
    EXPECT_EQ(ra.engine, "a");
    auto none = pool.chat(userMessage("z", "gamma"));
    EXPECT_FALSE(none.ok);
    EXPECT_EQ(none.status, 503);
    const auto models = pool.models();
    EXPECT_EQ(models, (std::vector<std::string>{"alpha", "beta"}));
}

TEST(EnginePool, FailsOverAndMarksUnhealthyAfterRepeatedFailures) {
    PoolOptions opts;
    opts.failuresBeforeUnhealthy = 2;
    EnginePool pool(opts);
    auto bad = std::make_shared<ScriptedBackend>("bad", true, std::vector<std::string>{"m"});
    auto good = std::make_shared<ScriptedBackend>("good", false, std::vector<std::string>{"m"});
    ASSERT_TRUE(pool.addEngine("bad", bad));
    ASSERT_TRUE(pool.addEngine("good", good));
    pool.probeNow();
    for (int i = 0; i < 3; ++i) {
        auto r = pool.chat(userMessage("q", "m"));
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.engine, "good");
    }
    EXPECT_EQ(bad->calls.load(), 2);
    const auto status = pool.status();
    ASSERT_EQ(status.size(), 2U);
    EXPECT_FALSE(status[0].healthy);
    EXPECT_EQ(status[0].failed, 2U);
    EXPECT_EQ(pool.healthyCount(), 1U);
}

TEST(EnginePool, PrefersLowerLatencyWhenIdle) {
    EnginePool pool;
    auto slow = std::make_shared<ScriptedBackend>("slow", false, std::vector<std::string>{"m"});
    auto fast = std::make_shared<ScriptedBackend>("fast", false, std::vector<std::string>{"m"});
    slow->latency = 500;
    fast->latency = 5;
    ASSERT_TRUE(pool.addEngine("a-slow", slow));
    ASSERT_TRUE(pool.addEngine("b-fast", fast));
    pool.probeNow();
    (void)pool.chat(userMessage("warm", "m"));
    (void)pool.chat(userMessage("warm", "m"));
    (void)pool.chat(userMessage("warm", "m"));
    const auto after = pool.chat(userMessage("q", "m"));
    EXPECT_EQ(after.engine, "b-fast");
}

TEST(EnginePool, PinOverridesRankingWhenEligible) {
    EnginePool pool;
    auto a = std::make_shared<ScriptedBackend>("a", false, std::vector<std::string>{"m"});
    auto b = std::make_shared<ScriptedBackend>("b", false, std::vector<std::string>{"m"});
    ASSERT_TRUE(pool.addEngine("a", a));
    ASSERT_TRUE(pool.addEngine("b", b));
    pool.probeNow();
    pool.pin("b");
    EXPECT_EQ(pool.chat(userMessage("x", "m")).engine, "b");
    EXPECT_EQ(pool.pinned().value_or(""), "b");
    ASSERT_TRUE(pool.setEnabled("b", false));
    EXPECT_EQ(pool.chat(userMessage("x", "m")).engine, "a");
}

TEST(EnginePool, LocalOnlyRequestsSkipRemoteEngines) {
    EnginePool pool;
    auto remote = std::make_shared<ScriptedBackend>("remote", false, std::vector<std::string>{"m"}, true);
    auto local = std::make_shared<ScriptedBackend>("local", false, std::vector<std::string>{"m"}, false);
    ASSERT_TRUE(pool.addEngine("a-remote", remote));
    ASSERT_TRUE(pool.addEngine("b-local", local));
    pool.probeNow();
    auto req = userMessage("x", "m");
    req.localOnly = true;
    for (int i = 0; i < 3; ++i) EXPECT_EQ(pool.chat(req).engine, "b-local");
    EXPECT_EQ(remote->calls.load(), 0);
}

TEST(EnginePool, ProbeMarksAndRecoversHealth) {
    EnginePool pool;
    auto flaky = std::make_shared<ScriptedBackend>("flaky", false, std::vector<std::string>{"m"});
    ASSERT_TRUE(pool.addEngine("flaky", flaky));
    pool.probeNow();
    EXPECT_EQ(pool.healthyCount(), 1U);
    flaky->probeFails = true;
    pool.probeNow();
    EXPECT_EQ(pool.healthyCount(), 0U);
    EXPECT_EQ(pool.status()[0].lastError, "probe failed");
    EXPECT_EQ(pool.chat(userMessage("x")).status, 503);
    flaky->probeFails = false;
    pool.probeNow();
    EXPECT_EQ(pool.healthyCount(), 1U);
    EXPECT_TRUE(pool.chat(userMessage("x")).ok);
}

TEST(EnginePool, RejectsDuplicateIdsAndSupportsRemoval) {
    EnginePool pool;
    auto a = std::make_shared<ScriptedBackend>("a", false, std::vector<std::string>{});
    std::string err;
    ASSERT_TRUE(pool.addEngine("x", a));
    EXPECT_FALSE(pool.addEngine("x", a, &err));
    EXPECT_FALSE(pool.addEngine("", a, &err));
    EXPECT_TRUE(pool.removeEngine("x"));
    EXPECT_FALSE(pool.removeEngine("x"));
    EXPECT_EQ(pool.size(), 0U);
}

TEST(EnginePool, BackgroundProbingDetectsOutages) {
    PoolOptions opts;
    opts.probeInterval = kBackendProbeInterval;
    opts.unhealthyProbeInterval = kBackendProbeInterval;
    EnginePool pool(opts);
    auto e = std::make_shared<ScriptedBackend>("e", false, std::vector<std::string>{"m"});
    ASSERT_TRUE(pool.addEngine("e", e));
    pool.startProbing();
    ASSERT_TRUE(support::waitUntil([&] { return !pool.status()[0].models.empty(); }));
    e->probeFails = true;
    EXPECT_TRUE(support::waitUntil([&] { return pool.healthyCount() == 0; }));
    e->probeFails = false;
    EXPECT_TRUE(support::waitUntil([&] { return pool.healthyCount() == 1; }));
    pool.stopProbing();
}

class ChatApiBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        log::setLevel(log::Level::Error);
        net::ServerOptions o;
        o.port = 0;
        server = std::make_unique<net::Server>(o);
        server->route("GET", "/v1/models", [this](net::Request& r) {
            lastAuth = r.header("Authorization");
            return net::Response::json(200, R"({"data":[{"id":"Llama3:Latest"},{"id":"qwen"}]})");
        });
        server->route("POST", "/v1/chat/completions", [this](net::Request& r) {
            lastAuth = r.header("Authorization");
            auto body = json::parse(r.body);
            lastBody = *body;
            if (mode == "error") return net::Response::error(429, "rate_limit", "slow down");
            if (mode == "notfound") return net::Response::error(404, "model_not_found", "no such model");
            if (mode == "badjson") return net::Response::json(200, "not json");
            if (body->getBool("stream")) {
                return net::Response::streaming(200, "text/event-stream", [](net::ChunkWriter& w) {
                    w.write("data: {\"model\":\"m\",\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n");
                    w.write("data: {\"choices\":[{\"delta\":{\"con");
                    w.write("tent\":\"lo\"}}]}\n\ndata: {\"choices\":[{\"delta\":{}}]}\n\ndata: [DONE]\n\n");
                });
            }
            return net::Response::json(
                200, R"({"model":"m","choices":[{"message":{"content":"hello"}}],"usage":{"prompt_tokens":3,"completion_tokens":1}})");
        });
        ASSERT_TRUE(server->start());
        ChatApiOptions o2;
        o2.baseUrl = "http://127.0.0.1:" + std::to_string(server->port()) + "/v1";
        o2.apiKey = "k123";
        o2.defaultModel = "fallback";
        backend = std::make_unique<ChatApiBackend>(o2);
    }

    std::unique_ptr<net::Server> server;
    std::unique_ptr<ChatApiBackend> backend;
    std::string mode = "ok";
    std::string lastAuth;
    json::Value lastBody;
};

TEST_F(ChatApiBackendTest, ChatSendsAuthAndParsesUsage) {
    auto r = backend->chat(userMessage("hi"));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.text, "hello");
    EXPECT_EQ(r.promptTokens, 3);
    EXPECT_EQ(r.completionTokens, 1);
    EXPECT_EQ(lastAuth, "Bearer k123");
    EXPECT_EQ(lastBody.getString("model"), "fallback");
    EXPECT_FALSE(lastBody.getBool("stream"));
}

TEST_F(ChatApiBackendTest, StreamReassemblesSplitEventsAndStopsOnDone) {
    std::string pieces;
    auto r = backend->chatStream(userMessage("hi", "m"), [&](std::string_view d) {
        pieces += std::string(d) + "|";
        return true;
    });
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.text, "Hello");
    EXPECT_EQ(pieces, "Hel|lo|");
    EXPECT_TRUE(lastBody.getBool("stream"));
}

TEST_F(ChatApiBackendTest, StreamStopsWhenCallbackDeclines) {
    int seen = 0;
    auto r = backend->chatStream(userMessage("hi", "m"), [&](std::string_view) { return ++seen < 1; });
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(seen, 1);
}

TEST_F(ChatApiBackendTest, ListsModels) {
    std::vector<std::string> models;
    std::string err;
    ASSERT_TRUE(backend->listModels(models, err)) << err;
    EXPECT_EQ(models, (std::vector<std::string>{"Llama3:Latest", "qwen"}));
}

TEST_F(ChatApiBackendTest, ClassifiesErrors) {
    mode = "error";
    auto limited = backend->chat(userMessage("x"));
    EXPECT_FALSE(limited.ok);
    EXPECT_TRUE(limited.retryable);
    EXPECT_EQ(limited.status, 429);
    EXPECT_EQ(limited.error, "slow down");
    mode = "notfound";
    EXPECT_TRUE(backend->chat(userMessage("x")).retryable);
    mode = "badjson";
    auto bad = backend->chat(userMessage("x"));
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(bad.retryable);
}

TEST_F(ChatApiBackendTest, PoolStreamsThroughAndCountsSuccess) {
    EnginePool pool;
    ChatApiOptions options;
    options.baseUrl = "http://127.0.0.1:" + std::to_string(server->port()) + "/v1";
    options.apiKey = "k";
    options.defaultModel = "m";
    ASSERT_TRUE(pool.addEngine("up", std::make_shared<ChatApiBackend>(options)));
    pool.probeNow();
    std::string text;
    auto r = pool.chatStream(userMessage("hi", "qwen"), [&](std::string_view d) {
        text += d;
        return true;
    });
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(text, "Hello");
    EXPECT_EQ(pool.status()[0].completed, 1U);
}
