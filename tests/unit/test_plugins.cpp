#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <fstream>

#include <gygax/core/log.hpp>
#include <gygax/net/http.hpp>
#include <gygax/service/plugins.hpp>
#include <gygax/service/service.hpp>

using namespace gygax;

namespace {

net::ClientResponse invoke(service::Service& svc, const std::string& tool, const std::string& input) {
    json::Value body = json::Value::object();
    body["input"] = input;
    return net::httpRequest(*net::Url::parse("http://127.0.0.1:" + std::to_string(svc.port()) + "/v1/tools/" + tool + "/invoke"), "POST",
                            body.dump(), {{"Authorization", "Bearer tok"}, {"Content-Type", "application/json"}});
}

service::ServiceConfig baseConfig() {
    log::setLevel(log::Level::Error);
    service::ServiceConfig c;
    c.port = 0;
    c.token = "tok";
    c.engines = {"echo"};
    return c;
}

} // namespace

TEST(Plugins, LoadsANativePluginAndExposesItsToolsToAgents) {
    std::string error;
    auto module = service::loadPlugin(GYGAX_TEST_PLUGIN_PATH, &error);
    ASSERT_TRUE(module.has_value()) << error;
    EXPECT_EQ(module->name, "geo");
    EXPECT_EQ(module->version, "1.0.0");
    ASSERT_EQ(module->tools.size(), 2U);
    EXPECT_EQ(module->tools[1].invoke("hello"), "hello");
    EXPECT_THROW(module->tools[0].invoke("garbage"), std::runtime_error);
    const auto meters = module->tools[0].invoke(R"({"from":{"latitude":0,"longitude":0},"to":{"latitude":0,"longitude":1}})");
    EXPECT_NEAR(std::stod(meters), 111195.0, 50.0);

    auto config = baseConfig();
    config.plugins = {GYGAX_TEST_PLUGIN_PATH};
    service::Service svc(config);
    ASSERT_TRUE(svc.start(&error)) << error;
    const auto ok = invoke(svc, "plugin.geo.echo", "ping");
    ASSERT_EQ(ok.status, 200) << ok.body;
    EXPECT_EQ(json::parse(ok.body)->getString("output"), "ping");
    EXPECT_EQ(invoke(svc, "plugin.geo.distance", "not json").status, 422);
    svc.stop();
    EXPECT_EQ(module->tools.size(), 2U);
}

TEST(Plugins, RejectsMissingBrokenAndUnsafePlugins) {
    std::string error;
    EXPECT_FALSE(service::loadPlugin("/nonexistent/plugin.so", &error));
    EXPECT_NE(error.find("not found"), std::string::npos);
    EXPECT_FALSE(service::loadPlugin("/etc/hostname", &error));

    char name[] = "/tmp/gygax-plugin-XXXXXX";
    const int fd = mkstemp(name);
    close(fd);
    std::ofstream(name) << "not an elf";
    EXPECT_FALSE(service::loadPlugin(name, &error));
    ASSERT_EQ(::chmod(name, 0666), 0);
    EXPECT_FALSE(service::loadPlugin(name, &error));
    EXPECT_NE(error.find("writable"), std::string::npos) << error;
    std::remove(name);

    auto config = baseConfig();
    config.plugins = {"/nonexistent/plugin.so"};
    service::Service svc(config);
    EXPECT_FALSE(svc.start(&error));
}

TEST(Plugins, RefusesToRegisterTheSamePluginTwice) {
    auto config = baseConfig();
    config.plugins = {GYGAX_TEST_PLUGIN_PATH, GYGAX_TEST_PLUGIN_PATH};
    service::Service svc(config);
    std::string error;
    EXPECT_FALSE(svc.start(&error));
    EXPECT_NE(error.find("already registered"), std::string::npos) << error;

    config.plugins = {GYGAX_TEST_PLUGIN_PATH};
    service::Service again(config);
    EXPECT_TRUE(again.start(&error)) << error;
}

TEST(Extensions, RegistersRemoteToolsAndPropagatesErrors) {
    net::ServerOptions so;
    so.host = "127.0.0.1";
    so.port = 0;
    so.workers = 2;
    net::Server ext(so);
    std::atomic<int> unauthorized{0};
    ext.route("GET", "/gygax/describe", [&](net::Request& req) {
        if (req.header("Authorization") != "Bearer secret") {
            ++unauthorized;
            return net::Response::error(401, "unauthorized", "bad token");
        }
        return net::Response::json(
            200,
            R"({"name":"ops","version":"0.1","tools":[{"name":"shout","description":"Upper-case the input"},{"name":"boom","description":"Always fails"}]})");
    });
    ext.route("POST", "/gygax/call/:tool", [](net::Request& req) {
        auto body = json::parse(req.body);
        if (req.params["tool"] == "boom") return net::Response::json(500, R"({"error":"exploded"})");
        std::string text = body->getString("input");
        for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        json::Value out = json::Value::object();
        out["output"] = text;
        return net::Response::json(200, out.dump());
    });
    std::string error;
    ASSERT_TRUE(ext.start(&error)) << error;
    const auto url = "http://127.0.0.1:" + std::to_string(ext.port());

    EXPECT_FALSE(service::connectExtension(url, &error));
    EXPECT_GT(unauthorized.load(), 0);

    auto config = baseConfig();
    config.extensions = {url + ";token=secret"};
    service::Service svc(config);
    ASSERT_TRUE(svc.start(&error)) << error;
    const auto shout = invoke(svc, "ext.ops.shout", "quiet");
    ASSERT_EQ(shout.status, 200) << shout.body;
    EXPECT_EQ(json::parse(shout.body)->getString("output"), "QUIET");
    const auto boom = invoke(svc, "ext.ops.boom", "x");
    EXPECT_EQ(boom.status, 422);
    EXPECT_NE(boom.body.find("exploded"), std::string::npos);
    svc.stop();
    ext.stop();
}

TEST(Extensions, RejectsInvalidSpecsAndDescriptors) {
    std::string error;
    EXPECT_FALSE(service::connectExtension("ftp://x", &error));
    EXPECT_FALSE(service::connectExtension("http://127.0.0.1:1;bogus=1", &error));
    EXPECT_FALSE(service::connectExtension("http://127.0.0.1:1", &error));
    EXPECT_TRUE(service::validPluginName("geo-1"));
    EXPECT_FALSE(service::validPluginName("Geo"));
    EXPECT_FALSE(service::validPluginName(""));
    EXPECT_TRUE(service::validPluginToolName("Do_it-2"));
    EXPECT_FALSE(service::validPluginToolName("a.b"));
}

TEST(Plugins, LoadsAPluginWrittenInPlainCThroughTheHeaderOnly) {
    std::string error;
    auto module = service::loadPlugin(GYGAX_TEST_C_PLUGIN_PATH, &error);
    ASSERT_TRUE(module.has_value()) << error;
    EXPECT_EQ(module->name, "cplug");
    ASSERT_EQ(module->tools.size(), 1U);
    EXPECT_EQ(module->tools[0].invoke("abc"), "cba");
    EXPECT_EQ(module->tools[0].invoke(""), "");
}
