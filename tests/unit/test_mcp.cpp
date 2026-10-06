#include <gtest/gtest.h>

#include <unistd.h>

#include <thread>

#include <gygax/core/log.hpp>
#include <gygax/inference/backend.hpp>
#include <gygax/net/http.hpp>
#include <gygax/service/mcp.hpp>
#include <gygax/service/service.hpp>

using namespace gygax;

namespace {

std::string fakeServerSpec(const std::string& name = "fake") {
    return name + "=python3 " + GYGAX_TEST_FAKE_MCP;
}

const service::ExternalTool* findTool(const service::ExternalModule& m, const std::string& name) {
    for (const auto& t : m.tools)
        if (t.name == name) return &t;
    return nullptr;
}

}

TEST(Mcp, ListsToolsAndCallsThem) {
    std::string error;
    auto module = service::connectMcp(fakeServerSpec(), &error);
    ASSERT_TRUE(module.has_value()) << error;
    EXPECT_EQ(module->kind, "mcp");
    EXPECT_EQ(module->name, "fake");
    EXPECT_EQ(module->version, "9.9");
    ASSERT_EQ(module->tools.size(), 3U);
    const auto* add = findTool(*module, "add");
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->description, "Add two numbers");
    EXPECT_EQ(add->invoke(R"({"a": 2, "b": 40})"), "42");
    EXPECT_EQ(findTool(*module, "echo")->invoke("plain words"), R"({"input": "plain words"})");
}

TEST(Mcp, SanitizesToolNamesAndSurfacesToolErrors) {
    auto module = service::connectMcp(fakeServerSpec(), nullptr);
    ASSERT_TRUE(module.has_value());
    const auto* failing = findTool(*module, "fail_now");
    ASSERT_NE(failing, nullptr);
    try {
        failing->invoke("{}");
        FAIL() << "expected an exception";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "boom");
    }
}

TEST(Mcp, RejectsBadSpecsAndDeadServers) {
    auto expectError = [](const std::string& spec, const char* needle) {
        std::string error;
        EXPECT_FALSE(service::connectMcp(spec, &error).has_value()) << spec;
        EXPECT_NE(error.find(needle), std::string::npos) << error;
    };
    expectError("no-equals", "NAME=COMMAND");
    expectError("Bad Name=python3", "name must be");
    expectError("x=", "missing command");
    expectError("x=/nonexistent/mcp-server", "exited");
}

TEST(Mcp, ServiceRegistersServerToolsForAgentsAndHttp) {
    log::setLevel(log::Level::Error);
    service::ServiceConfig c;
    c.port = 0;
    c.token = "tok";
    c.engines = {"echo"};
    c.mcpServers = {fakeServerSpec("calc")};
    service::Service svc(c);
    std::string error;
    ASSERT_TRUE(svc.start(&error)) << error;
    json::Value body = json::Value::object();
    body["input"] = R"({"a": 5, "b": 6})";
    auto r = net::httpRequest(*net::Url::parse("http://127.0.0.1:" + std::to_string(svc.port()) + "/v1/tools/mcp.calc.add/invoke"), "POST",
                              body.dump(), {{"Authorization", "Bearer tok"}, {"Content-Type", "application/json"}});
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_NE(r.body.find("11"), std::string::npos) << r.body;
    svc.stop();
}

TEST(EnginePresets, ResolveToTheDefaultPortsOfOllamaLlamaCppAndLmStudio) {
    using inference::resolveEngineSpec;
    EXPECT_EQ(resolveEngineSpec("ollama"), "http://127.0.0.1:11434/v1");
    EXPECT_EQ(resolveEngineSpec("llama-cpp"), "http://127.0.0.1:8080/v1");
    EXPECT_EQ(resolveEngineSpec("llama.cpp"), "http://127.0.0.1:8080/v1");
    EXPECT_EQ(resolveEngineSpec("lmstudio"), "http://127.0.0.1:1234/v1");
    EXPECT_EQ(resolveEngineSpec("ollama@10.0.0.5:9999;model=qwen"), "http://10.0.0.5:9999/v1;model=qwen");
    EXPECT_EQ(resolveEngineSpec("http://host:1/v1;key=k"), "http://host:1/v1;key=k");
    EXPECT_EQ(resolveEngineSpec("echo"), "echo");
}

TEST(EnginePresets, EachPresetTalksToAChatApiServer) {
    log::setLevel(log::Level::Error);
    for (const char* flavour : {"ollama", "llama-cpp", "lmstudio"}) {
        net::ServerOptions o;
        o.port = 0;
        net::Server server(o);
        std::string lastModel;
        server.route("GET", "/v1/models", [&](net::Request&) { return net::Response::json(200, R"({"data":[{"id":"local-model"}]})"); });
        server.route("POST", "/v1/chat/completions", [&](net::Request& r) {
            lastModel = json::parse(r.body)->getString("model");
            return net::Response::json(200, R"({"model":"local-model","choices":[{"message":{"content":"pong"}}]})");
        });
        ASSERT_TRUE(server.start());
        std::string error;
        auto backend =
            inference::makeBackend(std::string(flavour) + "@127.0.0.1:" + std::to_string(server.port()) + ";model=local-model", &error);
        ASSERT_NE(backend, nullptr) << flavour << ": " << error;
        inference::ChatRequest req;
        req.messages = {{"user", "ping"}};
        const auto result = backend->chat(req);
        ASSERT_TRUE(result.ok) << flavour << ": " << result.error;
        EXPECT_EQ(result.text, "pong");
        EXPECT_EQ(lastModel, "local-model");
        std::vector<std::string> models;
        ASSERT_TRUE(backend->listModels(models, error));
        EXPECT_EQ(models, std::vector<std::string>{"local-model"});
    }
}

TEST(McpServer, AnswersAClientOverPipes) {
    int toServer[2];
    int fromServer[2];
    ASSERT_EQ(::pipe(toServer), 0);
    ASSERT_EQ(::pipe(fromServer), 0);
    service::McpServerHandler handler;
    handler.list = [] { return std::vector<service::McpServerTool>{{"math.double", "Doubles a number"}}; };
    handler.call = [](const std::string& name, const std::string& args) -> std::string {
        if (name != "math.double") throw std::runtime_error("no such tool");
        return std::to_string(json::parse(args)->getInt("n") * 2);
    };
    std::thread server([&] { service::serveMcp(toServer[0], fromServer[1], "test", "1.0", handler); });
    auto exchange = [&](const std::string& request) {
        const std::string line = request + "\n";
        EXPECT_EQ(::write(toServer[1], line.data(), line.size()), static_cast<ssize_t>(line.size()));
        std::string out;
        char c;
        while (::read(fromServer[0], &c, 1) == 1 && c != '\n') out += c;
        return *json::parse(out);
    };
    auto init = exchange(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})");
    EXPECT_EQ(init.find("result")->getString("protocolVersion"), "2025-03-26");
    EXPECT_EQ(init.find("result")->find("serverInfo")->getString("name"), "test");
    ASSERT_EQ(::write(toServer[1], "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n", 55), 55);
    auto list = exchange(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    EXPECT_EQ(list.find("result")->find("tools")->asArray()[0].getString("name"), "math.double");
    auto call = exchange(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"math.double","arguments":{"n":21}}})");
    EXPECT_EQ(call.find("result")->find("content")->asArray()[0].getString("text"), "42");
    EXPECT_FALSE(call.find("result")->getBool("isError"));
    auto bad = exchange(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"nope"}})");
    EXPECT_TRUE(bad.find("result")->getBool("isError"));
    EXPECT_EQ(exchange(R"({"jsonrpc":"2.0","id":5,"method":"weird"})").find("error")->getInt("code"), -32601);
    EXPECT_EQ(exchange("not json").find("error")->getInt("code"), -32700);
    ::close(toServer[1]);
    server.join();
    ::close(toServer[0]);
    ::close(fromServer[0]);
    ::close(fromServer[1]);
}
