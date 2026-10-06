module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <gygax/core/json.hpp>
#include <gygax/core/log.hpp>
#include <gygax/inference/atari.hpp>
#include <gygax/inference/backend.hpp>

export module gygax.orchestration;

import gygax.world_model;
import gygax.signals;
import gygax.tools;

export namespace gygax::inference {

inline constexpr std::uint32_t kOrchestratorEpisodicMemoryCapacity = 4096;
inline constexpr std::size_t kRecentMemoryEntriesInPrompt = 5;
inline constexpr std::size_t kMaxPromptMemoryEntryBytes = 300;

struct AgentSnapshot {
    uint32_t sid = 0;
    std::string name;
    std::string status;
    std::string objective;
    std::string answer;
    std::string error;
    std::string model;
    uint32_t steps = 0;
    uint32_t maxSteps = 0;
    uint64_t runId = 0;
    std::size_t memoryEntries = 0;
    int64_t createdMs = 0;
    int64_t updatedMs = 0;
};

enum class SubmitResult { Accepted, UnknownAgent, Busy, Stopped, Invalid };

// Json is the default, established tool-calling protocol:
// {"tool": "<name>", "input": "<string>"} / {"answer": "<text>"}. Atari is
// the compact ATARI::CALL(...)/ATARI::DONE(...) grammar (see
// gygax/inference/atari.hpp and docs/ATARI.md), which interns tool names
// into short codes for the lifetime of one agent run; it is opt-in and only
// changes the wire format between the orchestrator and the model, not the
// tools or the state machine.
enum class ToolProtocol { Json, Atari };

struct OrchestratorOptions {
    std::size_t workers = 4;
    uint32_t defaultMaxSteps = 8;
    uint32_t maxStepsCeiling = 64;
    std::size_t maxQueued = 1024;
    std::size_t maxToolOutputBytes = 8192;
    std::string defaultModel;
    ToolProtocol protocol = ToolProtocol::Json;
};

struct OrchestratorStats {
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint64_t failed = 0;
    uint64_t toolCalls = 0;
    uint64_t modelCalls = 0;
    std::size_t queued = 0;
    std::size_t running = 0;
    std::size_t agents = 0;
};

struct ParsedAction {
    std::optional<std::string> tool;
    std::string input;
    std::string answer;
};

ParsedAction parseAction(const std::string& text) {
    ParsedAction out;
    out.answer = text;
    std::size_t start = text.find('{');
    while (start != std::string::npos) {
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        std::size_t end = std::string::npos;
        for (std::size_t i = start; i < text.size(); ++i) {
            const char c = text[i];
            if (inString) {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    inString = false;
                continue;
            }
            if (c == '"')
                inString = true;
            else if (c == '{')
                ++depth;
            else if (c == '}' && --depth == 0) {
                end = i;
                break;
            }
        }
        if (end == std::string::npos) break;
        if (auto parsed = json::parse(std::string_view(text).substr(start, end - start + 1)); parsed && parsed->isObject()) {
            if (const auto* tool = parsed->find("tool"); tool != nullptr && tool->isString() && !tool->asString().empty()) {
                out.tool = tool->asString();
                const auto* input = parsed->find("input");
                out.input = input == nullptr ? std::string() : (input->isString() ? input->asString() : input->dump());
                return out;
            }
            if (const auto* answer = parsed->find("answer"); answer != nullptr) {
                out.answer = answer->isString() ? answer->asString() : answer->dump();
                return out;
            }
        }
        start = text.find('{', start + 1);
    }
    return out;
}

ParsedAction parseAtariAction(const std::string& text, const atari::Schema& schema) {
    ParsedAction out;
    out.answer = text;
    const atari::Frame frame = atari::parse(text);
    if (frame.type == atari::FrameType::Call) {
        const std::string code = frame.get("t");
        out.tool = schema.resolve(code).value_or(code);
        out.input = frame.get("i");
    } else if (frame.type == atari::FrameType::Done) {
        out.answer = frame.get("a");
    }
    return out;
}

class Orchestrator {
public:
    Orchestrator(std::shared_ptr<Backend> backend, OrchestratorOptions options = {})
        : backend_(std::move(backend)), options_(std::move(options)) {}

    ~Orchestrator() { stop(); }

    Orchestrator(const Orchestrator&) = delete;
    Orchestrator& operator=(const Orchestrator&) = delete;

    void start() {
        std::lock_guard lock(mutex_);
        if (running_) return;
        running_ = true;
        const std::size_t count = std::max<std::size_t>(1, options_.workers);
        for (std::size_t i = 0; i < count; ++i) workers_.emplace_back([this] { workerLoop(); });
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            if (!running_) return;
            running_ = false;
            for (auto& [sid, rec] : records_) rec->cancel.store(true);
        }
        queueReady_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
        workers_.clear();
        std::lock_guard lock(mutex_);
        queue_.clear();
        doneReady_.notify_all();
    }

    uint32_t createAgent(std::string name, uint32_t maxSteps = 0) {
        auto& world = state::WorldModel::getInstance();
        const auto sid = world.spawnAgent();
        state::LatentSpace space;
        space.maxSteps = std::clamp<uint32_t>(maxSteps == 0 ? options_.defaultMaxSteps : maxSteps, 1, options_.maxStepsCeiling);
        world.updateRepresentation(sid, std::move(space));
        world.updateRepresentation(
            sid, state::EpisodicMemory{{}, "gygax agent", kOrchestratorEpisodicMemoryCapacity});
        auto rec = std::make_shared<Record>();
        rec->name = name.empty() ? std::format("agent-{}", sid) : std::move(name);
        rec->createdMs = nowMs();
        rec->updatedMs = rec->createdMs;
        {
            std::lock_guard lock(mutex_);
            records_[sid] = std::move(rec);
        }
        return sid;
    }

    bool removeAgent(uint32_t sid) {
        std::shared_ptr<Record> rec;
        {
            std::lock_guard lock(mutex_);
            auto it = records_.find(sid);
            if (it == records_.end()) return false;
            rec = it->second;
            if (rec->active) rec->cancel.store(true);
            records_.erase(it);
        }
        auto& world = state::WorldModel::getInstance();
        world.removeRepresentation<state::LatentSpace>(sid);
        world.removeRepresentation<state::EpisodicMemory>(sid);
        return true;
    }

    SubmitResult submit(uint32_t sid, std::string objective, std::string model = {}) {
        if (objective.empty()) return SubmitResult::Invalid;
        std::lock_guard lock(mutex_);
        if (!running_) return SubmitResult::Stopped;
        auto it = records_.find(sid);
        if (it == records_.end()) return SubmitResult::UnknownAgent;
        auto& rec = *it->second;
        if (rec.active || queue_.size() >= options_.maxQueued) return SubmitResult::Busy;
        rec.active = true;
        rec.cancel.store(false);
        rec.model = model.empty() ? options_.defaultModel : std::move(model);
        rec.updatedMs = nowMs();
        const auto runId = ++runCounter_;
        state::WorldModel::getInstance().mutate<state::LatentSpace>(sid, [&](state::LatentSpace& s) {
            s.objective = objective;
            s.answer.clear();
            s.error.clear();
            s.status = state::RunStatus::Running;
            s.stepCount = 0;
            s.runId = runId;
        });
        queue_.push_back(Job{sid, std::move(objective), runId, it->second});
        ++stats_.submitted;
        signals::GlobalEventRelay::Broadcast(signals::UserObjective{sid, queue_.back().objective});
        queueReady_.notify_one();
        return SubmitResult::Accepted;
    }

    bool cancel(uint32_t sid) {
        std::lock_guard lock(mutex_);
        auto it = records_.find(sid);
        if (it == records_.end() || !it->second->active) return false;
        it->second->cancel.store(true);
        return true;
    }

    [[nodiscard]] std::optional<AgentSnapshot> inspect(uint32_t sid) const {
        std::shared_ptr<Record> rec;
        {
            std::lock_guard lock(mutex_);
            auto it = records_.find(sid);
            if (it == records_.end()) return std::nullopt;
            rec = it->second;
        }
        return snapshotOf(sid, *rec);
    }

    [[nodiscard]] std::vector<AgentSnapshot> agents() const {
        std::vector<std::pair<uint32_t, std::shared_ptr<Record>>> all;
        {
            std::lock_guard lock(mutex_);
            all.assign(records_.begin(), records_.end());
        }
        std::vector<AgentSnapshot> out;
        out.reserve(all.size());
        for (const auto& [sid, rec] : all) out.push_back(snapshotOf(sid, *rec));
        return out;
    }

    [[nodiscard]] std::vector<std::string> memory(uint32_t sid, std::size_t tail = 100) const {
        auto mem = state::WorldModel::getInstance().snapshot<state::EpisodicMemory>(sid);
        if (!mem) return {};
        auto& logs = mem->eventLogs;
        if (logs.size() > tail) logs.erase(logs.begin(), logs.end() - static_cast<std::ptrdiff_t>(tail));
        return std::move(logs);
    }

    std::optional<AgentSnapshot> wait(uint32_t sid, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        auto it = records_.find(sid);
        if (it == records_.end()) return std::nullopt;
        auto rec = it->second;
        doneReady_.wait_for(lock, timeout, [&] { return !rec->active || !running_; });
        lock.unlock();
        return snapshotOf(sid, *rec);
    }

    [[nodiscard]] OrchestratorStats stats() const {
        std::lock_guard lock(mutex_);
        OrchestratorStats s = stats_;
        s.queued = queue_.size();
        s.agents = records_.size();
        s.running = static_cast<std::size_t>(std::ranges::count_if(records_, [](const auto& kv) { return kv.second->active; })) - s.queued;
        return s;
    }

    [[nodiscard]] const std::shared_ptr<Backend>& backend() const { return backend_; }

private:
    struct Record {
        std::string name;
        std::string model;
        bool active = false;
        std::atomic<bool> cancel{false};
        int64_t createdMs = 0;
        int64_t updatedMs = 0;
    };

    struct Job {
        uint32_t sid;
        std::string objective;
        uint64_t runId;
        std::shared_ptr<Record> record;
    };

    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    AgentSnapshot snapshotOf(uint32_t sid, const Record& rec) const {
        AgentSnapshot snap;
        snap.sid = sid;
        {
            std::lock_guard lock(mutex_);
            snap.name = rec.name;
            snap.model = rec.model;
            snap.createdMs = rec.createdMs;
            snap.updatedMs = rec.updatedMs;
        }
        auto& world = state::WorldModel::getInstance();
        if (auto space = world.snapshot<state::LatentSpace>(sid)) {
            snap.status = state::toString(space->status);
            snap.objective = space->objective;
            snap.answer = space->answer;
            snap.error = space->error;
            snap.steps = space->stepCount;
            snap.maxSteps = space->maxSteps;
            snap.runId = space->runId;
        }
        if (auto mem = world.snapshot<state::EpisodicMemory>(sid)) snap.memoryEntries = mem->eventLogs.size();
        return snap;
    }

    std::string systemPrompt(uint32_t sid, atari::Schema* schema) const {
        const auto tools = tools::ToolRegistry::getInstance().list();
        std::string prompt;
        if (schema == nullptr) {
            prompt =
                std::format("You are a Gygax autonomous agent. {}\n"
                            "Solve the user's objective. You may call tools. To call a tool respond with ONLY a JSON object "
                            "{{\"tool\": \"<name>\", \"input\": \"<string>\"}}. When done respond with ONLY {{\"answer\": \"<text>\"}}.\n"
                            "Available tools:\n",
                            EchoBackendMarker());
            if (tools.empty()) prompt += "(none)\n";
            for (const auto& t : tools)
                prompt += std::format("- {}: {}\n", t.name, t.description.empty() ? "no description" : t.description);
        } else {
            for (const auto& t : tools) schema->intern(t.name);
            prompt = std::format("You are a Gygax autonomous agent. {}\n"
                                 "Solve the user's objective. Respond with ONLY one frame: ATARI::CALL(t=<code>;i=<input>) to call a "
                                 "tool, or ATARI::DONE(a=<answer>) when finished. Tool codes:\n{}\nTools:\n",
                                 EchoBackendMarker(), schema->declareFrame());
            if (tools.empty()) prompt += "(none)\n";
            for (const auto& t : tools)
                prompt += std::format("- {}: {}\n", schema->intern(t.name), t.description.empty() ? "no description" : t.description);
        }
        if (auto mem = state::WorldModel::getInstance().snapshot<state::EpisodicMemory>(sid); mem && !mem->eventLogs.empty()) {
            prompt += "Recent memory:\n";
            const std::size_t from = mem->eventLogs.size() > kRecentMemoryEntriesInPrompt
                                         ? mem->eventLogs.size() - kRecentMemoryEntriesInPrompt
                                         : 0;
            for (std::size_t i = from; i < mem->eventLogs.size(); ++i)
                prompt += "- " + mem->eventLogs[i].substr(0, kMaxPromptMemoryEntryBytes) + "\n";
        }
        return prompt;
    }

    static std::string EchoBackendMarker() { return std::string(EchoBackend::kProtocolMarker); }

    void remember(uint32_t sid, std::string entry) {
        state::WorldModel::getInstance().mutate<state::EpisodicMemory>(sid, [&](state::EpisodicMemory& m) { m.append(std::move(entry)); });
    }

    void finish(const Job& job, state::RunStatus status, std::string answer, std::string error) {
        state::WorldModel::getInstance().mutate<state::LatentSpace>(job.sid, [&](state::LatentSpace& s) {
            if (s.runId != job.runId) return;
            s.status = status;
            s.answer = std::move(answer);
            s.error = std::move(error);
        });
        std::lock_guard lock(mutex_);
        job.record->active = false;
        job.record->updatedMs = nowMs();
        if (status == state::RunStatus::Completed)
            ++stats_.completed;
        else
            ++stats_.failed;
        doneReady_.notify_all();
    }

    void runJob(const Job& job) {
        auto& world = state::WorldModel::getInstance();
        auto space = world.snapshot<state::LatentSpace>(job.sid);
        if (!space) {
            finish(job, state::RunStatus::Failed, {}, "agent state missing");
            return;
        }
        std::string model;
        {
            std::lock_guard lock(mutex_);
            model = job.record->model;
        }
        remember(job.sid, "Objective: " + job.objective);

        atari::Schema schema;
        atari::Schema* const atariSchema = options_.protocol == ToolProtocol::Atari ? &schema : nullptr;

        ChatRequest request;
        request.model = model;
        request.messages.push_back({"system", systemPrompt(job.sid, atariSchema)});
        request.messages.push_back({"user", job.objective});

        for (uint32_t step = 1; step <= space->maxSteps; ++step) {
            if (job.record->cancel.load()) {
                remember(job.sid, "Cancelled");
                finish(job, state::RunStatus::Failed, {}, "cancelled");
                return;
            }
            world.mutate<state::LatentSpace>(job.sid, [&](state::LatentSpace& s) { s.stepCount = step; });
            {
                std::lock_guard lock(mutex_);
                ++stats_.modelCalls;
            }
            const ChatResult result = backend_->chat(request);
            if (job.record->cancel.load()) {
                remember(job.sid, "Cancelled");
                finish(job, state::RunStatus::Failed, {}, "cancelled");
                return;
            }
            if (!result.ok) {
                remember(job.sid, "Model error: " + result.error);
                finish(job, state::RunStatus::Failed, {}, "model error: " + result.error);
                return;
            }
            remember(job.sid, "Prediction: " + result.text);
            signals::GlobalEventRelay::Broadcast(signals::ModelPrediction{job.sid, result.text});

            const ParsedAction action = atariSchema != nullptr ? parseAtariAction(result.text, schema) : parseAction(result.text);
            if (!action.tool) {
                remember(job.sid, "Answer: " + action.answer);
                finish(job, state::RunStatus::Completed, action.answer, {});
                return;
            }

            if (job.record->cancel.load()) {
                remember(job.sid, "Cancelled");
                finish(job, state::RunStatus::Failed, {}, "cancelled");
                return;
            }
            signals::GlobalEventRelay::Broadcast(signals::ActionCall{job.sid, *action.tool, action.input});
            bool ok = true;
            std::string output;
            try {
                if (auto executed = tools::ToolRegistry::getInstance().execute(*action.tool, action.input)) {
                    output = std::move(*executed);
                } else {
                    ok = false;
                    output = "error: unknown tool '" + *action.tool + "'";
                }
            } catch (const std::exception& e) {
                ok = false;
                output = std::string("error: ") + e.what();
            }
            if (output.size() > options_.maxToolOutputBytes) {
                output.resize(options_.maxToolOutputBytes);
                output += "...[truncated]";
            }
            {
                std::lock_guard lock(mutex_);
                ++stats_.toolCalls;
            }
            remember(job.sid, std::format("Action {}({}) -> {}", *action.tool, action.input, output));
            request.messages.push_back({"assistant", result.text});
            if (atariSchema != nullptr) {
                const std::string code = atariSchema->intern(*action.tool);
                request.messages.push_back({"user", ok ? atari::encodeResult(code, output) : atari::encodeError(code, output)});
            } else {
                request.messages.push_back({"user", std::format("Tool result ({}): {}", *action.tool, output)});
            }
        }
        remember(job.sid, "Step limit reached");
        finish(job, state::RunStatus::Failed, {}, "step limit reached");
    }

    void workerLoop() {
        while (true) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                queueReady_.wait(lock, [this] { return !queue_.empty() || !running_; });
                if (!running_) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                runJob(job);
            } catch (const std::exception& e) {
                log::error("orchestrator", "run {} on agent {} crashed: {}", job.runId, job.sid, e.what());
                finish(job, state::RunStatus::Failed, {}, std::string("internal error: ") + e.what());
            }
        }
    }

    std::shared_ptr<Backend> backend_;
    OrchestratorOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable queueReady_;
    std::condition_variable doneReady_;
    std::map<uint32_t, std::shared_ptr<Record>> records_;
    std::deque<Job> queue_;
    std::vector<std::thread> workers_;
    OrchestratorStats stats_;
    uint64_t runCounter_ = 0;
    bool running_ = false;
};

}
