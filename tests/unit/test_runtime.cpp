#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <gygax/core/errors.hpp>
#include <gygax/core/json.hpp>
#include <gygax/core/log.hpp>
#include <gygax/core/object.hpp>
#include <gygax/inference/atari.hpp>
#include <gygax/inference/backend.hpp>

#include "support.hpp"

import gygax.core.base;
import gygax.core.messaging;
import gygax.world_model;
import gygax.signals;
import gygax.tools;
import gygax.orchestration;
import gygax.hardware.core;
import gygax.hardware.brains;

using namespace gygax;

namespace {

constexpr auto kAgentWaitTimeout = std::chrono::seconds(5);

}

TEST(WorldModel, SnapshotMutateAndRemove) {
    auto& world = state::WorldModel::getInstance();
    const auto sid = world.spawnAgent();
    EXPECT_FALSE(world.snapshot<state::LatentSpace>(sid));
    EXPECT_FALSE(world.mutate<state::LatentSpace>(sid, [](state::LatentSpace&) {}));
    world.updateRepresentation(sid, state::LatentSpace{});
    EXPECT_TRUE(world.mutate<state::LatentSpace>(sid, [](state::LatentSpace& s) { s.objective = "goal"; }));
    EXPECT_EQ(world.snapshot<state::LatentSpace>(sid)->objective, "goal");
    world.removeRepresentation<state::LatentSpace>(sid);
    EXPECT_FALSE(world.snapshot<state::LatentSpace>(sid));
}

TEST(WorldModel, EpisodicMemoryHonoursCapacity) {
    state::EpisodicMemory mem;
    mem.capacity = 3;
    for (int i = 0; i < 10; ++i) mem.append("e" + std::to_string(i));
    ASSERT_EQ(mem.eventLogs.size(), 3U);
    EXPECT_EQ(mem.eventLogs.front(), "e7");
    EXPECT_EQ(mem.eventLogs.back(), "e9");
}

TEST(WorldModel, SwarmMergeDeduplicatesKnowledgeAndAgents) {
    auto& world = state::WorldModel::getInstance();
    const auto host = world.spawnSwarm("host");
    const auto guest = world.spawnSwarm("guest");
    EXPECT_TRUE(world.addKnowledge(host, "a"));
    EXPECT_TRUE(world.addKnowledge(host, "b"));
    EXPECT_TRUE(world.addKnowledge(guest, "b"));
    EXPECT_TRUE(world.addKnowledge(guest, "c"));
    EXPECT_TRUE(world.assignAgentToSwarm(1, host));
    EXPECT_TRUE(world.assignAgentToSwarm(2, guest));
    EXPECT_FALSE(world.assignAgentToSwarm(3, 987654));
    EXPECT_FALSE(world.mergeSwarms(host, host));
    EXPECT_FALSE(world.mergeSwarms(host, 987654));
    ASSERT_TRUE(world.mergeSwarms(host, guest));
    const auto merged = world.swarmSnapshot(host);
    ASSERT_TRUE(merged);
    EXPECT_EQ(merged->globalKnowledgeBase, (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(merged->activeAgents.size(), 2U);
}

TEST(Events, FanOutToEverySubscriberWithoutStealing) {
    auto a = signals::GlobalEventRelay::subscribe();
    auto b = signals::GlobalEventRelay::subscribe();
    signals::GlobalEventRelay::Broadcast(signals::UserObjective{1, "go"});
    auto ea = a->poll();
    auto eb = b->poll();
    ASSERT_TRUE(ea && eb);
    EXPECT_EQ(std::get<signals::UserObjective>(*ea).query, "go");
    EXPECT_EQ(std::get<signals::UserObjective>(*eb).query, "go");
    EXPECT_FALSE(a->poll());
    signals::GlobalEventRelay::unsubscribe(a);
    signals::GlobalEventRelay::unsubscribe(b);
}

TEST(Events, BoundedQueueDropsOldestAndCounts) {
    auto sub = signals::GlobalEventRelay::subscribe(2);
    for (int i = 0; i < 5; ++i) signals::GlobalEventRelay::Broadcast(signals::ModelPrediction{1, std::to_string(i)});
    EXPECT_EQ(sub->pending(), 2U);
    EXPECT_EQ(sub->dropped(), 3U);
    EXPECT_EQ(std::get<signals::ModelPrediction>(*sub->poll()).rawJson, "3");
    signals::GlobalEventRelay::unsubscribe(sub);
}

TEST(Events, WaitForWakesOnDelivery) {
    auto sub = signals::GlobalEventRelay::subscribe();
    std::thread t([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        signals::GlobalEventRelay::Broadcast(signals::WebConstructionRequest{"x"});
    });
    auto ev = sub->waitFor(std::chrono::seconds(2));
    t.join();
    ASSERT_TRUE(ev);
    EXPECT_TRUE(std::holds_alternative<signals::WebConstructionRequest>(*ev));
    EXPECT_FALSE(sub->waitFor(std::chrono::milliseconds(10)));
    signals::GlobalEventRelay::unsubscribe(sub);
}

TEST(Tools, RegisterExecuteAndList) {
    auto& reg = tools::ToolRegistry::getInstance();
    reg.registerTool(
        "t.upper",
        [](const std::string& s) {
            std::string out = s;
            for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return out;
        },
        "uppercases");
    EXPECT_TRUE(reg.has("t.upper"));
    EXPECT_EQ(reg.execute("t.upper", "abc").value(), "ABC");
    EXPECT_FALSE(reg.execute("t.missing", ""));
    bool listed = false;
    for (const auto& t : reg.list()) listed = listed || (t.name == "t.upper" && t.description == "uppercases");
    EXPECT_TRUE(listed);
    EXPECT_TRUE(reg.unregisterTool("t.upper"));
    EXPECT_FALSE(reg.has("t.upper"));
}

TEST(Tools, ExceptionsAreWrappedAndTools_CanReenter) {
    auto& reg = tools::ToolRegistry::getInstance();
    reg.registerTool("t.throws", [](const std::string&) -> std::string { throw std::runtime_error("bad input"); });
    reg.registerTool("t.outer", [&reg](const std::string& s) { return reg.execute("t.inner", s).value_or("none"); });
    reg.registerTool("t.inner", [](const std::string& s) { return "inner:" + s; });
    EXPECT_THROW((void)reg.execute("t.throws", ""), ToolException);
    EXPECT_EQ(reg.execute("t.outer", "x").value(), "inner:x");
    reg.unregisterTool("t.throws");
    reg.unregisterTool("t.outer");
    reg.unregisterTool("t.inner");
}

namespace {

class Listener : public BaseObject {
public:
    explicit Listener(std::string name) : BaseObject(std::move(name), Capability::Communication) {}
    void onMessage(const std::string& sender, const std::string& msg) override { messages.push_back(sender + ">" + msg); }
    void onEvent(const std::string& type, const std::string& sender, const std::string& data) override {
        events.push_back(type + "@" + sender + ":" + data);
    }
    std::vector<std::string> messages;
    std::vector<std::string> events;
};

}

TEST(Hub, DeliversDirectAndBuffersForOfflineTargets) {
    Listener a("hub-a");
    a.initialize();
    a.sendMessage("hub-late", "queued while offline");
    Listener late("hub-late");
    late.initialize();
    ASSERT_EQ(late.messages.size(), 1U);
    EXPECT_EQ(late.messages[0], "hub-a>queued while offline");
    a.sendMessage("hub-late", "direct");
    EXPECT_EQ(late.messages.back(), "hub-a>direct");
    late.pre_delete();
    a.pre_delete();
}

TEST(Hub, EventsSkipTheSenderAndAllowReentrantSends) {
    Listener a("hub-e1");
    Listener b("hub-e2");
    a.initialize();
    b.initialize();
    a.triggerEvent("alert", "smoke");
    EXPECT_TRUE(a.events.empty());
    ASSERT_EQ(b.events.size(), 1U);
    EXPECT_EQ(b.events[0], "alert@hub-e1:smoke");
    class Echoer : public Listener {
    public:
        using Listener::Listener;
        void onMessage(const std::string& sender, const std::string& msg) override {
            if (msg == "ping") sendMessage(sender, "pong");
        }
    };
    Echoer echo("hub-echo");
    echo.initialize();
    a.sendMessage("hub-echo", "ping");
    ASSERT_FALSE(a.messages.empty());
    EXPECT_EQ(a.messages.back(), "hub-echo>pong");
    echo.pre_delete();
    a.pre_delete();
    b.pre_delete();
}

TEST(BaseObject, FrozenObjectsIgnoreParameterWritesAndThoughtsCompact) {
    BaseObject o("frozen-test", Capability::Cognition);
    o.setParameter("k", 1);
    o.freeze(true);
    o.setParameter("k", 2);
    EXPECT_EQ(std::get<int>(*o.getParameter("k")), 1);
    o.reset();
    o.setParameter("k", 3);
    EXPECT_EQ(std::get<int>(*o.getParameter("k")), 3);

    o.addGoal("stay alive");
    o.setMaxThoughts(4);
    for (int i = 0; i < 6; ++i) o.pushThought("thought " + std::to_string(i));
    ASSERT_LE(o.thoughts().size(), 4U);
    EXPECT_NE(o.thoughts().front().find("stay alive"), std::string::npos);
}

TEST(Capabilities, CombineAndCompare) {
    hardware::Node n("cap-node", Capability::Perception | Capability::Cognition);
    EXPECT_TRUE(n.compatible(Capability::Perception));
    EXPECT_FALSE(n.compatible(Capability::Actuation));
    hardware::Node m("cap-node-2", Capability::Actuation);
    EXPECT_FALSE(n.compatible(m));
    EXPECT_NE(n.diff(m).find("delta"), std::string::npos);
}

TEST(HardwareNode, DescribeReportsHierarchyAndParameters) {
    hardware::Node parent("hw-parent", Capability::Cognition);
    parent.setParameter("gain", 2.5f);
    parent.addInterface("Bus", hardware::Medium::Electrical);
    parent.addSubNode(std::make_unique<hardware::Node>("hw-child", Capability::Perception));
    const auto d = parent.describe();
    EXPECT_EQ(d.getString("name"), "hw-parent");
    EXPECT_DOUBLE_EQ(d.find("parameters")->getDouble("gain"), 2.5);
    ASSERT_EQ(d.find("children")->asArray().size(), 1U);
    EXPECT_EQ(d.find("children")->asArray()[0].getString("name"), "hw-child");
    EXPECT_EQ(d.find("interfaces")->asArray()[0].getString("medium"), "Electrical");
    EXPECT_EQ(parent.findNode("hw-child")->name(), "hw-child");
    EXPECT_EQ(parent.findNode("absent"), nullptr);
}

TEST(NeuromorphicNode, EncodesDecodesAndStepsWithTheNodeClock) {
    hardware::NeuromorphicNode node("snn", 0.1, 4);
    std::string err;
    const auto spec = *json::parse(R"({
      "dt": 0.1, "seed": 4,
      "populations": [
        {"name": "in", "type": "poisson", "n": 2, "rate": 0},
        {"name": "out", "type": "lif", "n": 2}
      ],
      "projections": [
        {"pre": "in", "post": "out", "connect": {"type": "one_to_one"}, "weight": 14.0, "delay_ms": 1.0}
      ]})");
    ASSERT_TRUE(node.configure(spec, &err)) << err;
    ASSERT_TRUE(node.bindInput("in"));
    ASSERT_TRUE(node.bindOutput("out"));
    EXPECT_TRUE(node.compatible(Capability::Neuromorphic));
    node.encode({1.0, 0.0}, 300.0);
    node.step(400.0);
    const auto counts = node.readout();
    ASSERT_EQ(counts.size(), 2U);
    EXPECT_GT(counts[0], counts[1]);
    EXPECT_EQ(node.decision(), 0U);
    EXPECT_GT(std::get<int>(*node.getParameter("spikes_total")), 0);
    EXPECT_NE(node.describe().find("network"), nullptr);
    node.freeze(true);
    const auto frozenTime = node.network().timeMs();
    node.step(100.0);
    EXPECT_DOUBLE_EQ(node.network().timeMs(), frozenTime);
}

namespace {

class Fixture : public ::testing::Test {
protected:
    void SetUp() override {
        log::setLevel(log::Level::Error);
        backend = std::make_shared<inference::EchoBackend>();
        inference::OrchestratorOptions o;
        o.workers = 3;
        o.defaultMaxSteps = 4;
        orch = std::make_unique<inference::Orchestrator>(backend, o);
        orch->start();
        auto& reg = tools::ToolRegistry::getInstance();
        reg.registerTool("o.double", [](const std::string& s) { return s + s; }, "doubles text");
        reg.registerTool("o.fail", [](const std::string&) -> std::string { throw std::runtime_error("tool exploded"); });
        reg.registerTool("o.loop", [](const std::string&) { return std::string("again"); });
    }

    void TearDown() override {
        orch->stop();
        auto& reg = tools::ToolRegistry::getInstance();
        reg.unregisterTool("o.double");
        reg.unregisterTool("o.fail");
        reg.unregisterTool("o.loop");
    }

    std::shared_ptr<inference::EchoBackend> backend;
    std::unique_ptr<inference::Orchestrator> orch;
};

}

TEST_F(Fixture, PlainObjectiveCompletes) {
    const auto sid = orch->createAgent("plain");
    ASSERT_EQ(orch->submit(sid, "hello"), inference::SubmitResult::Accepted);
    auto snap = orch->wait(sid, kAgentWaitTimeout);
    ASSERT_TRUE(snap);
    EXPECT_EQ(snap->status, "completed");
    EXPECT_EQ(snap->answer, "echo: hello");
    EXPECT_EQ(snap->steps, 1U);
}

TEST_F(Fixture, ToolLoopFeedsResultsBackIntoTheModel) {
    const auto sid = orch->createAgent("tools");
    ASSERT_EQ(orch->submit(sid, "!tool o.double ab"), inference::SubmitResult::Accepted);
    auto snap = orch->wait(sid, kAgentWaitTimeout);
    ASSERT_TRUE(snap);
    EXPECT_EQ(snap->status, "completed");
    EXPECT_EQ(snap->answer, "abab");
    EXPECT_EQ(snap->steps, 2U);
    const auto memory = orch->memory(sid);
    bool sawAction = false;
    for (const auto& m : memory) sawAction = sawAction || m.find("Action o.double(ab) -> abab") != std::string::npos;
    EXPECT_TRUE(sawAction);
    EXPECT_EQ(orch->stats().toolCalls, 1U);
}

TEST_F(Fixture, AtariProtocolDrivesAToolCallAndTheFinalAnswer) {
    using gygax::inference::atari::FrameType;
    class AtariBackend final : public inference::Backend {
    public:
        [[nodiscard]] std::string kind() const override { return "atari-fake"; }
        [[nodiscard]] std::string endpoint() const override { return "atari-fake"; }
        inference::ChatResult chat(const inference::ChatRequest& request) override {
            inference::ChatResult r;
            r.ok = true;
            r.status = 200;
            if (calls++ == 0) {
                systemPrompt = request.messages.front().content;
                const auto schemaAt = systemPrompt.find("ATARI::SCHEMA(");
                EXPECT_NE(schemaAt, std::string::npos);
                EXPECT_EQ(systemPrompt.find("{\"tool\""), std::string::npos);
                const auto schema = gygax::inference::atari::parse(systemPrompt.substr(schemaAt));
                EXPECT_EQ(schema.type, FrameType::Schema);
                for (const auto& [code, name] : schema.fields)
                    if (name == "o.double") toolCode = code;
                EXPECT_FALSE(toolCode.empty());
                r.text = gygax::inference::atari::encodeCall(toolCode, "ab");
            } else {
                lastToolResult = request.messages.back().content;
                const auto result = gygax::inference::atari::parse(lastToolResult);
                EXPECT_EQ(result.type, FrameType::Result);
                EXPECT_EQ(result.get("t"), toolCode);
                r.text = gygax::inference::atari::encodeDone(result.get("o") + "!");
            }
            return r;
        }
        bool listModels(std::vector<std::string>&, std::string&) override { return true; }

        int calls = 0;
        std::string toolCode;
        std::string systemPrompt;
        std::string lastToolResult;
    };
    auto backend = std::make_shared<AtariBackend>();
    inference::OrchestratorOptions options;
    options.protocol = inference::ToolProtocol::Atari;
    inference::Orchestrator atariOrch(backend, options);
    atariOrch.start();
    const auto sid = atariOrch.createAgent("atari");
    ASSERT_EQ(atariOrch.submit(sid, "double ab"), inference::SubmitResult::Accepted);
    auto snap = atariOrch.wait(sid, kAgentWaitTimeout);
    ASSERT_TRUE(snap);
    EXPECT_EQ(snap->status, "completed");
    EXPECT_EQ(snap->answer, "abab!");
    EXPECT_EQ(backend->calls, 2);
    EXPECT_EQ(backend->lastToolResult, gygax::inference::atari::encodeResult(backend->toolCode, "abab"));
    atariOrch.stop();
}

TEST_F(Fixture, AtariProtocolReportsToolFailuresAsErrorFrames) {
    using gygax::inference::atari::FrameType;
    class AtariBackend final : public inference::Backend {
    public:
        [[nodiscard]] std::string kind() const override { return "atari-fake"; }
        [[nodiscard]] std::string endpoint() const override { return "atari-fake"; }
        inference::ChatResult chat(const inference::ChatRequest& request) override {
            inference::ChatResult r;
            r.ok = true;
            r.status = 200;
            if (calls++ == 0) {
                const auto& sys = request.messages.front().content;
                const auto schemaAt = sys.find("ATARI::SCHEMA(");
                const auto schema = gygax::inference::atari::parse(sys.substr(schemaAt));
                for (const auto& [code, name] : schema.fields)
                    if (name == "o.fail") toolCode = code;
                r.text = gygax::inference::atari::encodeCall(toolCode, "x");
            } else {
                const auto result = gygax::inference::atari::parse(request.messages.back().content);
                EXPECT_EQ(result.type, FrameType::Error);
                EXPECT_NE(result.get("e").find("tool exploded"), std::string::npos);
                r.text = gygax::inference::atari::encodeDone("recovered: " + result.get("e"));
            }
            return r;
        }
        bool listModels(std::vector<std::string>&, std::string&) override { return true; }
        int calls = 0;
        std::string toolCode;
    };
    auto backend = std::make_shared<AtariBackend>();
    inference::OrchestratorOptions options;
    options.protocol = inference::ToolProtocol::Atari;
    inference::Orchestrator atariOrch(backend, options);
    atariOrch.start();
    const auto sid = atariOrch.createAgent("atari-fail");
    ASSERT_EQ(atariOrch.submit(sid, "fail please"), inference::SubmitResult::Accepted);
    auto snap = atariOrch.wait(sid, kAgentWaitTimeout);
    ASSERT_TRUE(snap);
    EXPECT_EQ(snap->status, "completed");
    EXPECT_NE(snap->answer.find("tool exploded"), std::string::npos);
    atariOrch.stop();
}

TEST_F(Fixture, ToolFailuresAreReportedToTheModelNotFatal) {
    const auto sid = orch->createAgent("failing");
    ASSERT_EQ(orch->submit(sid, "!tool o.fail x"), inference::SubmitResult::Accepted);
    auto snap = orch->wait(sid, kAgentWaitTimeout);
    ASSERT_TRUE(snap);
    EXPECT_EQ(snap->status, "completed");
    EXPECT_NE(snap->answer.find("tool exploded"), std::string::npos);
}

TEST_F(Fixture, UnknownToolIsAnErrorResultNotACrash) {
    const auto sid = orch->createAgent("unknown");
    ASSERT_EQ(orch->submit(sid, "!tool nope x"), inference::SubmitResult::Accepted);
    auto snap = orch->wait(sid, kAgentWaitTimeout);
    ASSERT_TRUE(snap);
    EXPECT_NE(snap->answer.find("unknown tool"), std::string::npos);
}

TEST_F(Fixture, StepLimitStopsRunawayAgents) {
    class LoopingBackend final : public inference::Backend {
    public:
        [[nodiscard]] std::string kind() const override { return "loop"; }
        [[nodiscard]] std::string endpoint() const override { return "loop"; }
        inference::ChatResult chat(const inference::ChatRequest&) override {
            inference::ChatResult r;
            r.ok = true;
            r.status = 200;
            r.text = R"({"tool":"o.loop","input":""})";
            return r;
        }
        bool listModels(std::vector<std::string>&, std::string&) override { return true; }
    };
    inference::Orchestrator looping(std::make_shared<LoopingBackend>());
    looping.start();
    const auto sid = looping.createAgent("runaway", 3);
    ASSERT_EQ(looping.submit(sid, "go"), inference::SubmitResult::Accepted);
    auto snap = looping.wait(sid, kAgentWaitTimeout);
    ASSERT_TRUE(snap);
    EXPECT_EQ(snap->status, "failed");
    EXPECT_EQ(snap->error, "step limit reached");
    EXPECT_EQ(snap->steps, 3U);
}

TEST_F(Fixture, ModelErrorsFailTheRunWithTheReason) {
    class Broken final : public inference::Backend {
    public:
        [[nodiscard]] std::string kind() const override { return "broken"; }
        [[nodiscard]] std::string endpoint() const override { return "broken"; }
        inference::ChatResult chat(const inference::ChatRequest&) override {
            inference::ChatResult r;
            r.error = "engine down";
            return r;
        }
        bool listModels(std::vector<std::string>&, std::string&) override { return true; }
    };
    inference::Orchestrator o(std::make_shared<Broken>());
    o.start();
    const auto sid = o.createAgent("b");
    ASSERT_EQ(o.submit(sid, "x"), inference::SubmitResult::Accepted);
    auto snap = o.wait(sid, kAgentWaitTimeout);
    EXPECT_EQ(snap->status, "failed");
    EXPECT_NE(snap->error.find("engine down"), std::string::npos);
}

TEST_F(Fixture, BusyUnknownAndInvalidSubmissions) {
    class Slow final : public inference::Backend {
    public:
        [[nodiscard]] std::string kind() const override { return "slow"; }
        [[nodiscard]] std::string endpoint() const override { return "slow"; }
        inference::ChatResult chat(const inference::ChatRequest&) override {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            inference::ChatResult r;
            r.ok = true;
            r.status = 200;
            r.text = R"({"answer":"done"})";
            return r;
        }
        bool listModels(std::vector<std::string>&, std::string&) override { return true; }
    };
    inference::Orchestrator o(std::make_shared<Slow>());
    o.start();
    const auto sid = o.createAgent("s");
    EXPECT_EQ(o.submit(sid, ""), inference::SubmitResult::Invalid);
    EXPECT_EQ(o.submit(424242, "x"), inference::SubmitResult::UnknownAgent);
    ASSERT_EQ(o.submit(sid, "x"), inference::SubmitResult::Accepted);
    EXPECT_EQ(o.submit(sid, "y"), inference::SubmitResult::Busy);
    auto snap = o.wait(sid, kAgentWaitTimeout);
    EXPECT_EQ(snap->answer, "done");
    EXPECT_EQ(o.submit(sid, "again"), inference::SubmitResult::Accepted);
    (void)o.wait(sid, kAgentWaitTimeout);
}

TEST_F(Fixture, CancelStopsAnAgentBetweenSteps) {
    class Stepper final : public inference::Backend {
    public:
        [[nodiscard]] std::string kind() const override { return "stepper"; }
        [[nodiscard]] std::string endpoint() const override { return "stepper"; }
        inference::ChatResult chat(const inference::ChatRequest&) override {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            inference::ChatResult r;
            r.ok = true;
            r.status = 200;
            r.text = R"({"tool":"o.loop","input":""})";
            return r;
        }
        bool listModels(std::vector<std::string>&, std::string&) override { return true; }
    };
    inference::Orchestrator o(std::make_shared<Stepper>());
    o.start();
    const auto sid = o.createAgent("c", 60);
    ASSERT_EQ(o.submit(sid, "go"), inference::SubmitResult::Accepted);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(o.cancel(sid));
    auto snap = o.wait(sid, kAgentWaitTimeout);
    EXPECT_EQ(snap->status, "failed");
    EXPECT_EQ(snap->error, "cancelled");
    EXPECT_FALSE(o.cancel(sid));
}

TEST_F(Fixture, CancelDuringModelCallDoesNotDispatchItsReturnedToolAction) {
    class BlockingBackend final : public inference::Backend {
    public:
        [[nodiscard]] std::string kind() const override { return "blocking"; }
        [[nodiscard]] std::string endpoint() const override { return "blocking"; }
        inference::ChatResult chat(const inference::ChatRequest&) override {
            std::unique_lock lock(mutex);
            entered = true;
            ready.notify_all();
            ready.wait(lock, [&] { return released; });
            inference::ChatResult result;
            result.ok = true;
            result.status = 200;
            result.text = R"({"tool":"o.cancel_probe","input":"must-not-run"})";
            return result;
        }
        bool listModels(std::vector<std::string>&, std::string&) override { return true; }

        std::mutex mutex;
        std::condition_variable ready;
        bool entered = false;
        bool released = false;
    };

    auto backend = std::make_shared<BlockingBackend>();
    std::atomic<int> toolCalls = 0;
    auto& registry = tools::ToolRegistry::getInstance();
    registry.registerTool("o.cancel_probe", [&](const std::string&) {
        ++toolCalls;
        return std::string("unexpected");
    });

    inference::Orchestrator o(backend);
    o.start();
    const auto sid = o.createAgent("cancel-in-flight");
    ASSERT_EQ(o.submit(sid, "cancel before the tool runs"), inference::SubmitResult::Accepted);

    bool entered = false;
    {
        std::unique_lock lock(backend->mutex);
        entered = backend->ready.wait_for(lock, kAgentWaitTimeout, [&] { return backend->entered; });
    }
    const bool cancelled = entered && o.cancel(sid);
    {
        std::lock_guard lock(backend->mutex);
        backend->released = true;
    }
    backend->ready.notify_all();

    const auto snapshot = o.wait(sid, kAgentWaitTimeout);
    o.stop();
    const bool unregistered = registry.unregisterTool("o.cancel_probe");
    EXPECT_TRUE(entered);
    EXPECT_TRUE(cancelled);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->error, "cancelled");
    EXPECT_EQ(toolCalls.load(), 0);
    EXPECT_TRUE(unregistered);
}

TEST_F(Fixture, ManyAgentsRunConcurrently) {
    std::vector<std::uint32_t> ids;
    for (int i = 0; i < 24; ++i) {
        ids.push_back(orch->createAgent("m" + std::to_string(i)));
        ASSERT_EQ(orch->submit(ids.back(), "job" + std::to_string(i)), inference::SubmitResult::Accepted);
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        auto snap = orch->wait(ids[i], std::chrono::seconds(10));
        ASSERT_TRUE(snap);
        EXPECT_EQ(snap->answer, "echo: job" + std::to_string(i));
    }
    EXPECT_EQ(orch->stats().completed, 24U);
    EXPECT_EQ(orch->agents().size(), 24U);
}

TEST_F(Fixture, ObserversSeeModelAndToolEvents) {
    auto sub = signals::GlobalEventRelay::subscribe();
    const auto sid = orch->createAgent("observed");
    ASSERT_EQ(orch->submit(sid, "!tool o.double z"), inference::SubmitResult::Accepted);
    (void)orch->wait(sid, kAgentWaitTimeout);
    int objectives = 0;
    int predictions = 0;
    int actions = 0;
    while (auto e = sub->poll()) {
        objectives += std::holds_alternative<signals::UserObjective>(*e) ? 1 : 0;
        predictions += std::holds_alternative<signals::ModelPrediction>(*e) ? 1 : 0;
        actions += std::holds_alternative<signals::ActionCall>(*e) ? 1 : 0;
    }
    signals::GlobalEventRelay::unsubscribe(sub);
    EXPECT_EQ(objectives, 1);
    EXPECT_EQ(predictions, 2);
    EXPECT_EQ(actions, 1);
}

TEST_F(Fixture, RemovedAgentsDisappear) {
    const auto sid = orch->createAgent("gone");
    EXPECT_TRUE(orch->inspect(sid));
    EXPECT_TRUE(orch->removeAgent(sid));
    EXPECT_FALSE(orch->inspect(sid));
    EXPECT_FALSE(orch->removeAgent(sid));
}

TEST(ParseAction, ExtractsToolCallsAnswersAndFallsBackToText) {
    auto call = inference::parseAction("Sure! ```json\n{\"tool\":\"t\",\"input\":{\"a\":1}}\n```");
    ASSERT_TRUE(call.tool);
    EXPECT_EQ(*call.tool, "t");
    EXPECT_EQ(call.input, R"({"a":1})");
    EXPECT_EQ(inference::parseAction(R"(noise {"answer":"42"} tail)").answer, "42");
    const auto plain = inference::parseAction("just text {not json");
    EXPECT_FALSE(plain.tool);
    EXPECT_EQ(plain.answer, "just text {not json");
    EXPECT_FALSE(inference::parseAction(R"({"other":1})").tool);
}
