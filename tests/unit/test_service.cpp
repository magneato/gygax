#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include <gygax/core/json.hpp>
#include <gygax/core/log.hpp>
#include <gygax/service/expression.hpp>
#include <gygax/service/node_info.hpp>
#include <gygax/service/service.hpp>
#include <gygax/version.hpp>

#include "support.hpp"

using namespace gygax;
using namespace gygax::service;

namespace {

class ServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        log::setLevel(log::Level::Error);
        ServiceConfig c;
        c.port = 0;
        c.token = "tok";
        c.engines = {"echo"};
        c.agentMaxSteps = 4;
        svc = std::make_unique<Service>(c);
        std::string err;
        ASSERT_TRUE(svc->start(&err)) << err;
        base = "http://127.0.0.1:" + std::to_string(svc->port());
    }

    void TearDown() override { svc->stop(); }

    net::ClientResponse call(const std::string& method, const std::string& path, const std::string& body = {}, bool auth = true,
                             net::Headers extra = {}) {
        net::Headers h = std::move(extra);
        h["Content-Type"] = "application/json";
        if (auth) h["Authorization"] = "Bearer tok";
        return net::httpRequest(*net::Url::parse(base + path), method, body, h);
    }

    json::Value jsonOf(const net::ClientResponse& r) {
        auto parsed = json::parse(r.body);
        EXPECT_TRUE(parsed) << r.body;
        return parsed ? *parsed : json::Value();
    }

    static std::string chatBody(const std::string& model, const std::string& content, bool stream = false) {
        json::Value b = json::Value::object();
        b["model"] = model;
        b["stream"] = stream;
        json::Value m = json::Value::object();
        m["role"] = "user";
        m["content"] = content;
        b["messages"].push(std::move(m));
        return b.dump();
    }

    std::unique_ptr<Service> svc;
    std::string base;
};

}

TEST_F(ServiceTest, HealthAndVersionArePublic) {
    EXPECT_EQ(call("GET", "/healthz", "", false).status, 200);
    EXPECT_EQ(call("GET", "/readyz", "", false).status, 200);
    EXPECT_EQ(jsonOf(call("GET", "/version", "", false)).getString("name"), "gygax");
}

TEST_F(ServiceTest, EverythingElseRequiresTheToken) {
    for (const char* path : {"/v1/models", "/v1/node", "/v1/agents", "/metrics", "/api/tags", "/status"}) {
        const auto r = call("GET", path, "", false);
        EXPECT_EQ(r.status, 401) << path;
        EXPECT_EQ(r.headers.at("WWW-Authenticate"), "Bearer");
    }
    EXPECT_EQ(call("GET", "/v1/models", "", false, {{"Authorization", "Bearer nope"}}).status, 401);
    EXPECT_EQ(call("GET", "/v1/models", "", false, {{"Authorization", "tok"}}).status, 401);
    EXPECT_EQ(call("GET", "/v1/models").status, 200);
}

TEST_F(ServiceTest, ModelsListEnginesAndTheAgentModel) {
    const auto models = jsonOf(call("GET", "/v1/models"));
    EXPECT_EQ(models.getString("object"), "list");
    std::vector<std::string> ids;
    for (const auto& m : models.find("data")->asArray()) ids.push_back(m.getString("id"));
    EXPECT_EQ(ids, (std::vector<std::string>{"echo", "gygax-agent"}));
    const auto forwarded = jsonOf(call("GET", "/v1/models", "", true, {{"X-Gygax-Forwarded", "1"}}));
    EXPECT_EQ(forwarded.find("data")->asArray().size(), 1U);
}

TEST_F(ServiceTest, ChatCompletionShapeMatchesTheStandardApi) {
    const auto r = call("POST", "/v1/chat/completions", chatBody("echo", "hi"));
    ASSERT_EQ(r.status, 200);
    const auto v = jsonOf(r);
    EXPECT_EQ(v.getString("object"), "chat.completion");
    EXPECT_TRUE(v.getString("id").starts_with("chatcmpl-"));
    const auto& choice = v.find("choices")->asArray()[0];
    EXPECT_EQ(choice.find("message")->getString("content"), "echo: hi");
    EXPECT_EQ(choice.find("message")->getString("role"), "assistant");
    EXPECT_EQ(choice.getString("finish_reason"), "stop");
    EXPECT_GT(v.find("usage")->getInt("total_tokens"), 0);
}

TEST_F(ServiceTest, ChatValidatesInput) {
    EXPECT_EQ(call("POST", "/v1/chat/completions", "{").status, 400);
    EXPECT_EQ(call("POST", "/v1/chat/completions", "[]").status, 400);
    EXPECT_EQ(call("POST", "/v1/chat/completions", R"({"model":"echo"})").status, 400);
    EXPECT_EQ(call("POST", "/v1/chat/completions", R"({"model":"echo","messages":[]})").status, 400);
    EXPECT_EQ(call("POST", "/v1/chat/completions", R"({"model":"echo","messages":[{"role":"wizard","content":"x"}]})").status, 400);
    EXPECT_EQ(call("POST", "/v1/chat/completions", chatBody("unknown-model", "x")).status, 200);
}

TEST_F(ServiceTest, ContentPartsAreAccepted) {
    const std::string body =
        R"({"model":"echo","messages":[{"role":"user","content":[{"type":"text","text":"part one "},{"type":"text","text":"two"}]}]})";
    const auto r = call("POST", "/v1/chat/completions", body);
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(jsonOf(r).find("choices")->asArray()[0].find("message")->getString("content"), "echo: part one two");
}

TEST_F(ServiceTest, StreamingProducesWellFormedSse) {
    const auto r = call("POST", "/v1/chat/completions", chatBody("echo", "stream me", true));
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.headers.at("Content-Type"), "text/event-stream");
    std::string content;
    bool done = false;
    std::size_t pos = 0;
    while (pos < r.body.size()) {
        const auto end = r.body.find("\n\n", pos);
        ASSERT_NE(end, std::string::npos);
        const auto line = r.body.substr(pos, end - pos);
        pos = end + 2;
        ASSERT_TRUE(line.starts_with("data: ")) << line;
        const auto payload = line.substr(6);
        if (payload == "[DONE]") {
            done = true;
            continue;
        }
        const auto chunk = *json::parse(payload);
        EXPECT_EQ(chunk.getString("object"), "chat.completion.chunk");
        content += chunk.find("choices")->asArray()[0].find("delta")->getString("content");
    }
    EXPECT_TRUE(done);
    EXPECT_EQ(content, "echo: stream me");
}

TEST_F(ServiceTest, AgentModelRunsTheToolLoop) {
    const auto r = call("POST", "/v1/chat/completions", chatBody("gygax-agent:echo", "!tool math.eval 2^10"));
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(jsonOf(r).find("choices")->asArray()[0].find("message")->getString("content"), "1024");
    const auto streamed = call("POST", "/v1/chat/completions", chatBody("gygax-agent", "!tool math.eval 3*3", true));
    EXPECT_NE(streamed.body.find("\"content\":\"9\""), std::string::npos);
    EXPECT_NE(streamed.body.find("[DONE]"), std::string::npos);
    const auto forwardedAgent = call("POST", "/v1/chat/completions", chatBody("gygax-agent", "x"), true, {{"X-Gygax-Forwarded", "1"}});
    EXPECT_EQ(forwardedAgent.status, 400);
}

TEST(AtariToolProtocol, RejectsAnUnknownProtocolName) {
    ServiceConfig c;
    c.toolProtocol = "morse";
    std::string error;
    EXPECT_FALSE(c.validate(error));
    EXPECT_NE(error.find("toolProtocol"), std::string::npos);
}

TEST(AtariToolProtocol, DrivesTheRealToolLoopThroughTheHttpService) {
    log::setLevel(log::Level::Error);
    ServiceConfig c;
    c.port = 0;
    c.token = "tok";
    c.engines = {"echo"};
    c.agentMaxSteps = 4;
    c.toolProtocol = "atari";
    Service svc(c);
    std::string err;
    ASSERT_TRUE(svc.start(&err)) << err;
    const std::string base = "http://127.0.0.1:" + std::to_string(svc.port());
    auto call = [&](const std::string& body) {
        net::Headers h{{"Content-Type", "application/json"}, {"Authorization", "Bearer tok"}};
        return net::httpRequest(*net::Url::parse(base + "/v1/chat/completions"), "POST", body, h);
    };
    json::Value b = json::Value::object();
    b["model"] = "gygax-agent:echo";
    b["stream"] = false;
    json::Value m = json::Value::object();
    m["role"] = "user";
    m["content"] = "!tool math.eval 2^10";
    b["messages"].push(std::move(m));
    const auto r = call(b.dump());
    ASSERT_EQ(r.status, 200) << r.body;
    const auto parsed = json::parse(r.body);
    ASSERT_TRUE(parsed) << r.body;
    EXPECT_EQ(parsed->find("choices")->asArray()[0].find("message")->getString("content"), "echo: 1024");
    svc.stop();
}

TEST_F(ServiceTest, OllamaCompatibleEndpoints) {
    const auto tags = jsonOf(call("GET", "/api/tags"));
    EXPECT_EQ(tags.find("models")->asArray()[0].getString("name"), "echo");
    json::Value chat = json::parse(chatBody("echo", "hey", false)).value();
    const auto once = jsonOf(call("POST", "/api/chat", chat.dump()));
    EXPECT_EQ(once.find("message")->getString("content"), "echo: hey");
    EXPECT_TRUE(once.getBool("done"));

    json::Value streamBody = chat;
    streamBody["stream"] = true;
    const auto lines = call("POST", "/api/chat", streamBody.dump());
    EXPECT_EQ(lines.headers.at("Content-Type"), "application/x-ndjson");
    std::istringstream in(lines.body);
    std::string line;
    std::string joined;
    bool sawDone = false;
    while (std::getline(in, line)) {
        const auto frame = *json::parse(line);
        joined += frame.find("message")->getString("content");
        sawDone = sawDone || frame.getBool("done");
    }
    EXPECT_TRUE(sawDone);
    EXPECT_EQ(joined, "echo: hey");

    const auto gen = jsonOf(call("POST", "/api/generate", R"({"model":"echo","prompt":"p","stream":false})"));
    EXPECT_EQ(gen.getString("response"), "echo: p");
    EXPECT_EQ(call("POST", "/api/generate", R"({"model":"echo"})").status, 400);
}

TEST_F(ServiceTest, AgentLifecycleOverRest) {
    const auto created = call("POST", "/v1/agents", R"({"name":"rest","max_steps":3})");
    ASSERT_EQ(created.status, 201);
    const auto sid = jsonOf(created).getInt("id");
    EXPECT_EQ(jsonOf(created).getInt("max_steps"), 3);

    const std::string path = "/v1/agents/" + std::to_string(sid);
    EXPECT_EQ(jsonOf(call("GET", path)).getString("status"), "idle");
    const auto done = call("POST", path + "/objectives", R"({"objective":"!tool math.eval 7*6","model":"echo","wait_seconds":10})");
    ASSERT_EQ(done.status, 200) << done.body;
    EXPECT_EQ(jsonOf(done).getString("answer"), "42");
    EXPECT_EQ(jsonOf(done).getInt("steps"), 2);
    const auto memory = jsonOf(call("GET", path + "/memory?tail=2"));
    EXPECT_EQ(memory.find("entries")->asArray().size(), 2U);
    EXPECT_EQ(call("POST", path + "/objectives", R"({"objective":""})").status, 400);
    EXPECT_EQ(call("POST", path + "/cancel").status, 409);
    EXPECT_EQ(call("GET", "/v1/agents/999999").status, 404);
    EXPECT_EQ(call("GET", "/v1/agents/notanumber").status, 400);
    EXPECT_EQ(jsonOf(call("GET", "/v1/agents")).find("agents")->asArray().size(), 1U);
    EXPECT_EQ(call("DELETE", path).status, 200);
    EXPECT_EQ(call("GET", path).status, 404);
}

TEST_F(ServiceTest, AsynchronousObjectivesReturn202ThenComplete) {
    const auto sid = jsonOf(call("POST", "/v1/agents", "{}")).getInt("id");
    const std::string path = "/v1/agents/" + std::to_string(sid);
    const auto accepted = call("POST", path + "/objectives", R"({"objective":"hello","model":"echo"})");
    EXPECT_TRUE(accepted.status == 200 || accepted.status == 202);
    ASSERT_TRUE(support::waitUntil([&] { return jsonOf(call("GET", path)).getString("status") == "completed"; }));
    EXPECT_EQ(jsonOf(call("GET", path)).getString("answer"), "echo: hello");
}

TEST_F(ServiceTest, ToolsListInvokeAndErrors) {
    const auto tools = jsonOf(call("GET", "/v1/tools"));
    std::set<std::string> names;
    for (const auto& t : tools.find("tools")->asArray()) names.insert(t.getString("name"));
    for (const char* n : {"math.eval", "time.now", "node.info", "neuro.run", "webpage.construct"}) EXPECT_TRUE(names.contains(n)) << n;
    EXPECT_EQ(jsonOf(call("POST", "/v1/tools/math.eval/invoke", R"({"input":"sqrt(81)+1"})")).getString("output"), "10");
    EXPECT_EQ(call("POST", "/v1/tools/math.eval/invoke", R"({"input":"1/0"})").status, 422);
    EXPECT_EQ(call("POST", "/v1/tools/nope/invoke", "{}").status, 404);
    const auto now = jsonOf(call("POST", "/v1/tools/time.now/invoke", "{}")).getString("output");
    EXPECT_EQ(now.size(), 20U);
    const auto info = jsonOf(call("POST", "/v1/tools/node.info/invoke", "{}"));
    EXPECT_TRUE(json::parse(info.getString("output")));
}

TEST_F(ServiceTest, NeuroSimulationEndpointAndTool) {
    const std::string spec = R"({"dt":0.1,"seed":1,"run_ms":200,"populations":[{"name":"a","type":"lif","n":3,"bias":25}]})";
    const auto direct = call("POST", "/v1/neuro/simulate", spec);
    ASSERT_EQ(direct.status, 200) << direct.body;
    EXPECT_GT(jsonOf(direct).find("results")->asArray()[0].getInt("spike_count"), 0);
    json::Value body = json::Value::object();
    body["input"] = *json::parse(spec);
    const auto viaTool = jsonOf(call("POST", "/v1/tools/neuro.run/invoke", body.dump()));
    EXPECT_GT(json::parse(viaTool.getString("output"))->find("results")->asArray()[0].getInt("spike_count"), 0);
    EXPECT_EQ(call("POST", "/v1/neuro/simulate", R"({"populations":[]})").status, 400);
}

TEST_F(ServiceTest, EngineManagement) {
    EXPECT_EQ(jsonOf(call("GET", "/v1/engines")).find("engines")->asArray().size(), 1U);
    EXPECT_EQ(call("POST", "/v1/engines", R"({"id":"echo","spec":"echo"})").status, 409);
    EXPECT_EQ(call("POST", "/v1/engines", R"({"id":"x","spec":"not a url"})").status, 400);
    EXPECT_EQ(call("POST", "/v1/engines", R"({"id":"second","spec":"echo"})").status, 201);
    EXPECT_EQ(call("POST", "/v1/engines/second/disable").status, 200);
    EXPECT_FALSE(jsonOf(call("GET", "/v1/engines")).find("engines")->asArray()[1].getBool("enabled"));
    EXPECT_EQ(call("POST", "/v1/engines/second/enable").status, 200);
    EXPECT_EQ(call("POST", "/v1/pin", R"({"id":"second"})").status, 200);
    EXPECT_EQ(jsonOf(call("GET", "/v1/engines")).getString("pinned"), "second");
    EXPECT_EQ(call("POST", "/v1/pin", R"({"id":"ghost"})").status, 404);
    EXPECT_EQ(call("POST", "/v1/pin", "{}").status, 200);
    EXPECT_EQ(call("DELETE", "/v1/engines/second").status, 200);
    EXPECT_EQ(call("DELETE", "/v1/engines/second").status, 404);
    EXPECT_EQ(call("POST", "/v1/probe").status, 200);
}

TEST_F(ServiceTest, NodeAndClusterInfo) {
    const auto node = jsonOf(call("GET", "/v1/node"));
    EXPECT_GT(node.getInt("cpu_cores"), 0);
    EXPECT_GT(node.getInt("memory_total_mb"), 0);
    EXPECT_EQ(node.getInt("engines_healthy"), 1);
    EXPECT_EQ(node.getString("version"), GYGAX_VERSION_STRING);
    const auto cluster = jsonOf(call("GET", "/v1/cluster"));
    EXPECT_TRUE(cluster.find("peers")->asArray().empty());
    EXPECT_NE(cluster.find("self"), nullptr);
}

TEST_F(ServiceTest, MetricsAreValidPrometheusText) {
    (void)call("POST", "/v1/chat/completions", chatBody("echo", "count me"));
    const auto text = call("GET", "/metrics").body;
    EXPECT_NE(text.find("# TYPE gygax_http_requests_total counter"), std::string::npos);
    EXPECT_NE(text.find("gygax_engine_healthy{engine=\"echo\"} 1"), std::string::npos);
    EXPECT_NE(text.find("gygax_chat_requests_total 1"), std::string::npos);
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto space = line.rfind(' ');
        ASSERT_NE(space, std::string::npos) << line;
        EXPECT_NO_THROW((void)std::stod(line.substr(space + 1))) << line;
    }
    EXPECT_EQ(text, svc->metricsText().substr(0, 0) + text);
}

TEST_F(ServiceTest, JsonRpcSingleBatchAndErrors) {
    auto rpc = [&](const std::string& body) { return jsonOf(call("POST", "/rpc", body)); };
    const auto ok = rpc(R"({"jsonrpc":"2.0","id":"a","method":"tools.list"})");
    EXPECT_EQ(ok.getString("id"), "a");
    EXPECT_NE(ok.find("result"), nullptr);
    const auto created = rpc(R"({"jsonrpc":"2.0","id":1,"method":"agents.create","params":{"name":"rpc"}})");
    const auto sid = created.find("result")->getInt("id");
    json::Value submit = json::Value::object();
    submit["jsonrpc"] = "2.0";
    submit["id"] = 2;
    submit["method"] = "agents.submit";
    submit["params"]["sid"] = sid;
    submit["params"]["objective"] = "!tool math.eval 5+5";
    submit["params"]["model"] = "echo";
    submit["params"]["wait_seconds"] = 10;
    EXPECT_EQ(rpc(submit.dump()).find("result")->getString("answer"), "10");
    const auto batch = rpc(R"([{"jsonrpc":"2.0","id":1,"method":"node.info"},{"jsonrpc":"2.0","id":2,"method":"nope"}])");
    ASSERT_TRUE(batch.isArray());
    EXPECT_NE(batch.asArray()[0].find("result"), nullptr);
    EXPECT_EQ(batch.asArray()[1].find("error")->getInt("code"), -32601);
    EXPECT_EQ(rpc(R"({"jsonrpc":"1.0","id":1,"method":"x"})").find("error")->getInt("code"), -32600);
    EXPECT_EQ(rpc(R"([])").find("error")->getInt("code"), -32600);
    EXPECT_EQ(rpc("{bad").find("error")->getInt("code"), -32700);
    const auto notFound = rpc(R"({"jsonrpc":"2.0","id":3,"method":"agents.get","params":{"sid":424242}})");
    EXPECT_EQ(notFound.find("error")->getInt("code"), -32000);
    EXPECT_EQ(notFound.find("error")->find("data")->getInt("status"), 404);
    EXPECT_EQ(svc->rpc(*json::parse(R"({"jsonrpc":"2.0","id":9,"method":"engines.list"})")).getInt("id"), 9);
}

TEST_F(ServiceTest, LegacyPortalEndpointsStillWork) {
    EXPECT_EQ(jsonOf(call("GET", "/status")).getString("status"), "online");
    EXPECT_EQ(call("POST", "/directive", "{}").status, 400);
    EXPECT_EQ(call("POST", "/directive", R"({"directive":"build the portal"})").status, 200);
    const auto status = jsonOf(call("GET", "/status"));
    EXPECT_NE(status.getString("latest_html").find("<canvas"), std::string::npos);
    EXPECT_NE(status.getString("latest_text").find("SYSTEM PROTOCOLS"), std::string::npos);
}

TEST_F(ServiceTest, ManyConcurrentClients) {
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    for (int i = 0; i < 32; ++i) {
        threads.emplace_back([&, i] {
            const auto r = call("POST", "/v1/chat/completions", chatBody("echo", "c" + std::to_string(i)));
            if (r.status == 200 && r.body.find("echo: c" + std::to_string(i)) != std::string::npos) ++ok;
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(ok.load(), 32);
}

TEST(ServiceConfig, ValidationRules) {
    std::string err;
    ServiceConfig c;
    EXPECT_TRUE(c.validate(err));
    c.host = "0.0.0.0";
    EXPECT_FALSE(c.validate(err));
    EXPECT_NE(err.find("GYGAX_API_TOKEN"), std::string::npos);
    c.token = "x";
    EXPECT_TRUE(c.validate(err));
    c.token.clear();
    c.allowInsecureRemote = true;
    EXPECT_TRUE(c.validate(err));
    c.allowInsecureRemote = false;
    c.host = "127.0.0.1";
    c.engines = {"http://ok/v1", "garbage"};
    EXPECT_FALSE(c.validate(err));
    c.engines.clear();
    c.peers = {"nope"};
    EXPECT_FALSE(c.validate(err));
    c.peers.clear();
    c.httpWorkers = 0;
    EXPECT_FALSE(c.validate(err));
    EXPECT_TRUE(isLoopbackHost("127.0.0.5"));
    EXPECT_TRUE(isLoopbackHost("localhost"));
    EXPECT_FALSE(isLoopbackHost("10.0.0.1"));
}

TEST(ServiceConfig, ReadsEnvironment) {
    ::setenv("GYGAX_PORT", "2001", 1);
    ::setenv("GYGAX_ENGINES", "echo, http://a:1/v1 ,", 1);
    ::setenv("GYGAX_API_TOKEN", "t0k", 1);
    ::setenv("GYGAX_RATE_LIMIT_PER_MINUTE", "30", 1);
    const auto c = ServiceConfig::fromEnvironment();
    EXPECT_EQ(c.port, 2001);
    EXPECT_EQ(c.engines, (std::vector<std::string>{"echo", "http://a:1/v1"}));
    EXPECT_EQ(c.token, "t0k");
    EXPECT_EQ(c.rateLimitPerMinute, 30U);
    for (const char* n : {"GYGAX_PORT", "GYGAX_ENGINES", "GYGAX_API_TOKEN", "GYGAX_RATE_LIMIT_PER_MINUTE"}) ::unsetenv(n);
}

TEST(ServiceConfig, ReadsTheTokenFromAFile) {
    const auto path = std::filesystem::temp_directory_path() / "gygax-token-test";
    {
        std::ofstream out(path);
        out << "file-token \r\n";
    }
    EXPECT_EQ(readTokenFile(path.string()), "file-token");
    EXPECT_EQ(readTokenFile("/nonexistent/gygax/token"), "");
    ::setenv("GYGAX_API_TOKEN_FILE", path.c_str(), 1);
    ::unsetenv("GYGAX_API_TOKEN");
    EXPECT_EQ(ServiceConfig::fromEnvironment().token, "file-token");
    ::unsetenv("GYGAX_API_TOKEN_FILE");
    std::filesystem::remove(path);
}

TEST(ServiceRateLimit, ReturnsTooManyRequests) {
    log::setLevel(log::Level::Error);
    ServiceConfig c;
    c.port = 0;
    c.engines = {"echo"};
    c.rateLimitPerMinute = 5;
    Service s(c);
    ASSERT_TRUE(s.start());
    int limited = 0;
    for (int i = 0; i < 8; ++i) {
        const auto r = net::httpRequest(*net::Url::parse("http://127.0.0.1:" + std::to_string(s.port()) + "/v1/models"), "GET");
        limited += r.status == 429 ? 1 : 0;
        if (r.status == 429) EXPECT_EQ(r.headers.at("Retry-After"), "60");
    }
    EXPECT_EQ(limited, 3);
    EXPECT_EQ(net::httpRequest(*net::Url::parse("http://127.0.0.1:" + std::to_string(s.port()) + "/healthz"), "GET").status, 200);
}

TEST(ServiceLifecycle, RefusesInsecureBindAndSupportsRpcOnly) {
    ServiceConfig insecure;
    insecure.host = "0.0.0.0";
    insecure.port = 0;
    Service a(insecure);
    std::string err;
    EXPECT_FALSE(a.start(&err));

    ServiceConfig rpcOnly;
    rpcOnly.enableHttp = false;
    rpcOnly.engines = {"echo"};
    Service b(rpcOnly);
    ASSERT_TRUE(b.start(&err)) << err;
    EXPECT_EQ(b.port(), 0);
    const auto reply = b.rpc(*json::parse(R"({"jsonrpc":"2.0","id":1,"method":"engines.list"})"));
    EXPECT_NE(reply.find("result"), nullptr);
    EXPECT_FALSE(b.metricsText().empty());
    b.stop();
    b.stop();
}

TEST(ServiceLifecycle, PortInUseIsReported) {
    ServiceConfig c;
    c.port = 0;
    c.engines = {"echo"};
    Service first(c);
    ASSERT_TRUE(first.start());
    c.port = first.port();
    Service second(c);
    std::string err;
    EXPECT_FALSE(second.start(&err));
    EXPECT_FALSE(err.empty());
}

TEST(Expression, EvaluatesArithmeticSafely) {
    double v = 0;
    std::string err;
    ASSERT_TRUE(evaluateExpression("2 + 3 * 4 - 1", v, err));
    EXPECT_DOUBLE_EQ(v, 13.0);
    ASSERT_TRUE(evaluateExpression("-(2 ^ 3) ^ 2", v, err));
    EXPECT_DOUBLE_EQ(v, -64.0);
    ASSERT_TRUE(evaluateExpression("max(min(5, 9), 2) % 3 + pi - pi", v, err));
    EXPECT_DOUBLE_EQ(v, 2.0);
    ASSERT_TRUE(evaluateExpression("ln(e) + sqrt(4) + abs(-1) + floor(1.9) + round(2.5)", v, err));
    EXPECT_DOUBLE_EQ(v, 1 + 2 + 1 + 1 + 3);
    for (const char* bad : {"", "1 +", "(1", "1/0", "sqrt(-1)", "ln(0)", "foo(1)", "1 2", "pow(1)", "2 $ 3", "unknown", "1e999999"}) {
        EXPECT_FALSE(evaluateExpression(bad, v, err)) << bad;
        EXPECT_FALSE(err.empty()) << bad;
    }
    EXPECT_FALSE(evaluateExpression(std::string(3000, '1'), v, err));
    EXPECT_FALSE(evaluateExpression(std::string(200, '(') + "1" + std::string(200, ')'), v, err));
}

TEST(NodeInfo, ParsesNvidiaSmiOutput) {
    const auto gpus = parseNvidiaSmi("NVIDIA GeForce RTX 4090, 24564, 1024, 37\nbroken line\nTesla T4, 15360, 0, 0\n");
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[0].name, "NVIDIA GeForce RTX 4090");
    EXPECT_EQ(gpus[0].memoryTotalMb, 24564);
    EXPECT_DOUBLE_EQ(gpus[0].utilizationPct, 37.0);
    EXPECT_TRUE(parseNvidiaSmi("").empty());
    const auto info = collectNodeInfo(false);
    EXPECT_FALSE(info.hostname.empty());
    EXPECT_GT(info.cpuCores, 0U);
    EXPECT_TRUE(toJson(info).contains("gpus"));
}
