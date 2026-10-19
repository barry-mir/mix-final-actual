#include "agent/Agent.h"
#include "agent/Tools.h"
#include "core/Paths.h"
#include <cstdlib>
#include <cmath>

namespace im
{
namespace
{
int triggerPriority (const std::string& k) { return k == "user" ? 3 : k == "stage" ? 2 : 1; }
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
} // namespace

Agent::Agent (StateStore& s, AgentHost& h, Bridge& b) : juce::Thread ("Agent"), store (s), host (h), bridge (b)
{
    logs.setDir (appDataDir().getChildFile ("logs"));
}

Agent::~Agent() { stop(); }

const char* Agent::stateName (State s)
{
    switch (s)
    {
        case State::Thinking: return "thinking";
        case State::Idle: return "idle";
        default: return "offline";
    }
}

void Agent::start()
{
    lastTickEndMs = nowMs();
    if (! isThreadRunning()) startThread (juce::Thread::Priority::normal);
}

void Agent::stop()
{
    if (isThreadRunning())
    {
        signalThreadShouldExit();
        bridge.cancel(); // release a turn that is waiting for the coding agent
        wake.signal();
        stopThread (5000);
    }
}

double Agent::thinkingElapsedSec() const
{
    return st.load() == State::Thinking ? (nowMs() - thinkingSince.load()) / 1000.0 : 0.0;
}

void Agent::trigger (Trigger t)
{
    if (store.settings().mode == AgentMode::Off) return;
    {
        std::lock_guard<std::mutex> lk (m);
        if (! pending) pending = t;
        else if (t.kind == "user" && pending->kind == "user")
            pending->msg += "\n" + t.msg; // never lose a user message
        else if (triggerPriority (t.kind) >= triggerPriority (pending->kind))
            pending = t;
    }
    wake.signal();
}

void Agent::userMessage (const std::string& text)
{
    ChatEntry e;
    e.role = "user";
    e.text = text;
    e.time = juce::Time::currentTimeMillis() / 1000.0;
    store.addChat (e);
    if (store.settings().mode == AgentMode::Off)
    {
        ChatEntry s;
        s.role = "system";
        s.text = "Assistant is Off. Switch to Assist or Auto to let it act.";
        s.time = e.time;
        store.addChat (s);
        return;
    }
    trigger ({ "user", text });
    queueTurns (kRequestFollowUps, "refine", text); // replaces the follow-ups of an earlier request
}

void Agent::queueTurns (int n, const std::string& label, const std::string& goal)
{
    {
        std::lock_guard<std::mutex> lk (m);
        queued = n;
        queuedLabel = label;
        queuedGoal = goal;
    }
    wake.signal();
}

const char* Agent::stageName (Stage s)
{
    switch (s)
    {
        case Stage::Listening: return "listening";
        case Stage::Waiting: return "waiting";
        case Stage::Applying: return "applying";
        case Stage::Done: return "done";
        default: return "none";
    }
}

void Agent::setStage (Stage s, int changes)
{
    stageNow = (int) s;
    stageChanges = changes;
    stageSinceMs = nowMs();
}

Agent::TurnStatus Agent::turnStatus() const
{
    TurnStatus t;
    t.tick = stageTick.load();
    t.stage = (Stage) stageNow.load();
    t.sinceSec = t.stage == Stage::None ? 0.0 : (nowMs() - stageSinceMs.load()) / 1000.0;
    t.changes = stageChanges.load();
    return t;
}

std::optional<Trigger> Agent::takePending()
{
    std::lock_guard<std::mutex> lk (m);
    auto p = pending;
    pending.reset();
    if (! p && queued > 0)
    {
        --queued;
        p = Trigger { queuedLabel == "refine" ? "refine" : queuedGoal.empty() ? "auto" : "goal", queuedGoal, queuedLabel };
    }
    return p;
}

void Agent::run()
{
    while (! threadShouldExit())
    {
        const auto cfg = store.settings();
        int waitMs = 500;
        if (cfg.mode == AgentMode::Auto)
        {
            const double due = lastTickEndMs + cfg.tickS * 1000.0;
            waitMs = (int) juce::jlimit (10.0, 500.0, due - nowMs());
        }
        wake.wait (waitMs);
        if (threadShouldExit()) break;

        if (st.load() != State::Thinking)
            st = bridge.secondsSinceAgentSeen() < 120.0 ? State::Idle : State::Offline;

        auto t = takePending();
        if (! t && store.settings().mode == AgentMode::Auto && nowMs() >= lastTickEndMs + store.settings().tickS * 1000.0)
            t = Trigger { "auto", {} };
        if (! t) continue;
        if (store.settings().mode == AgentMode::Off) continue;
        runTick (*t);
        lastTickEndMs = nowMs();
    }
}

bool Agent::runTick (const Trigger& trig)
{
    const auto cfg = store.settings();
    const double t0 = nowMs();
    LatencyRow lat;
    lat.trigger = trig.kind;

    st = State::Thinking;
    thinkingSince = nowMs();
    const int tick = store.nextTickNumber();
    lat.tick = tick;
    stageTick = tick;
    stopRequested = false;

    // 1. listen: the measurements must describe the mix as it is now (live: an offline render of the analysis range)
    setStage (Stage::Listening);
    host.listen ([this] { return threadShouldExit() || stopRequested.load(); });
    if (bridge.secondsSinceAgentSeen() > 60.0 && ! (autoAgentAvailable && autoAgentAvailable()))
    {
        ChatEntry s;
        s.role = "system";
        s.tick = tick;
        s.text = "Waiting for your coding agent. Ask it to connect to the mixer (its mix tools), and this turn starts.";
        s.time = juce::Time::currentTimeMillis() / 1000.0;
        store.addChat (s);
    }

    auto snap = host.snapshot();
    const TrackMask mask = host.presentMask();

    // 5. features_after of the previous record (master + tracks it touched)
    {
        auto last = store.lastRecords (1);
        if (! last.empty() && last.back().featuresAfter.is_null())
            store.setFeaturesAfterForLast (compactFeatures (snap.get(), tracksTouched (last.back().applied)));
    }

    // 2. prompt
    PromptInputs in;
    in.names = store.trackNames();
    in.stage = store.stagePositions();
    in.presentMask = mask;
    in.locked = store.lockedParams();
    in.memory = store.lastRecords (cfg.memoryK);
    in.paramValue = [this] (int i) { return host.paramValue (i); };
    in.groupBusGr = [this] (int g) { return host.groupBusGrDb (g); };
    in.groupBusGain = [this] (int g) { return host.groupBusGainDb (g); };
    in.snapshot = snap;
    in.trigger = trig.kind;
    in.userMsg = trig.msg;
    in.tokenBudget = cfg.promptTokenBudget;
    in.promptVersion = cfg.promptVersion;
    const auto prompt = buildPrompt (in);
    lat.estPromptTokens = prompt.estTokens;

    // 3. request (+ one retry on malformed JSON)
    json messages = prompt.messages;
    ParsedResponse parsed;
    json lastReply;
    std::string failure;
    double prefill = 0, decode = 0;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (stopRequested.exchange (false)) // the user pressed Stop while the console was listening
        {
            failure = "cancelled";
            break;
        }
        const json request = { { "messages", messages }, { "tools", toolSchemas() } };
        const auto label = attempt > 0 ? std::string ("retry") : trig.label.empty() ? trig.kind : trig.label;
        setStage (Stage::Waiting);
        auto res = bridge.ask (request, label, trig.msg, turnTimeoutS.load(), [this, tick] (const std::string& text) {
            ChatEntry n;
            n.role = "note";
            n.tick = tick;
            n.text = text;
            n.time = juce::Time::currentTimeMillis() / 1000.0;
            store.addChat (n);
        });
        logs.transcript (tick, attempt, request, res.ok ? res.response : json ({ { "error", res.error } }));
        if (! res.ok)
        {
            failure = res.error;
            break;
        }
        parsed = parseChatResponse (res.response);
        lastReply = res.response;
        prefill += parsed.promptMs;
        decode += parsed.predictedMs;
        lat.promptTokens = parsed.promptTokens;
        lat.completionTokens += parsed.completionTokens;
        if (parsed.parseError.empty())
        {
            failure.clear();
            break;
        }
        failure = "malformed tool call: " + parsed.parseError;
        if (attempt == 0)
        {
            ++lat.retries;
            const auto& msg = res.response["choices"][0].value ("message", json::object());
            messages.push_back ({ { "role", "assistant" }, { "content", msg.value ("content", json ("")).is_string() ? msg.value ("content", std::string()) : std::string() } });
            messages.push_back ({ { "role", "user" },
                                  { "content", "Error: " + parsed.parseError + ". Reply again with a short rationale and valid JSON tool calls." } });
        }
    }
    lat.prefillMs = prefill;
    lat.decodeMs = decode;

    MemoryRecord rec;
    rec.tick = tick;
    rec.time = juce::Time::currentTimeMillis() / 1000.0;
    rec.trigger = trig.kind;
    rec.userMsg = trig.msg;

    if (! failure.empty())
    {
        lat.totalMs = nowMs() - t0;
        lat.status = "skipped";
        logs.latency (lat);
        rec.rationale = "(tick skipped: " + failure.substr (0, 160) + ")";
        rec.featuresBefore = compactFeatures (snap.get(), {});
        rec.latencyTotalMs = lat.totalMs;
        store.addRecord (rec);
        ChatEntry s;
        s.role = "system";
        s.tick = tick;
        s.text = failure == "cancelled" ? "Stopped; nothing more was changed."
                                        : "Skipped: " + failure.substr (0, 500);
        s.time = rec.time;
        store.addChat (s);
        setStage (Stage::Done, 0);
        st = State::Idle;
        return false;
    }

    // validate
    ToolContext ctx;
    ctx.names = in.names;
    ctx.presentMask = mask;
    ctx.currentValue = [this] (int i) { return host.paramValue (i); };
    ctx.isLocked = [this] (int i) { return store.isLocked (i); };
    ctx.settings = cfg;
    ctx.userRequest = trig.kind == "user" || trig.kind == "refine";
    const auto val = validateToolCalls (parsed.calls, ctx);

    // apply (message thread, LLM ramp)
    setStage (Stage::Applying, (int) val.targets.size());
    if (! val.targets.empty()) host.applyAgentChanges (tick, val.targets, cfg.llmRampS);

    // record + chat
    rec.rationale = parsed.content.empty() && val.noChange ? val.noChangeReason : parsed.content;
    rec.applied = val.changes;
    for (auto& c : parsed.calls) rec.toolCalls.push_back ({ { "name", c.name }, { "arguments", c.args } });
    rec.featuresBefore = compactFeatures (snap.get(), tracksTouched (val.changes));
    lat.totalMs = nowMs() - t0;
    lat.status = val.targets.empty() ? (val.noChange ? "no_change" : "nothing_applied") : "applied";
    lat.numCalls = (int) parsed.calls.size();
    rec.latencyPrefillMs = prefill;
    rec.latencyDecodeMs = decode;
    rec.latencyTotalMs = lat.totalMs;
    store.addRecord (rec);

    ChatEntry e;
    e.role = "agent";
    e.tick = tick;
    e.time = rec.time;
    e.text = rec.rationale.empty() ? "(no rationale)" : rec.rationale;
    if (val.noChange && ! val.noChangeReason.empty() && val.targets.empty() && ! parsed.content.empty())
        e.text += " [no change: " + val.noChangeReason + "]";
    for (auto& c : val.changes)
        if (c.status != "superseded" && c.status != "unchanged") e.diff.push_back (c);
    for (auto& err : val.errors) e.text += "\n(" + err + ")";
    {
        const int nTracks = mask.count();
        char b[256];
        std::snprintf (b, sizeof (b), "read %d tracks + mix (3 s features), %d memory turn%s, %d prompt tokens; %d tool call%s; %.1f s",
                       nTracks, prompt.memoryUsed, prompt.memoryUsed == 1 ? "" : "s", lat.promptTokens > 0 ? lat.promptTokens : prompt.estTokens,
                       (int) parsed.calls.size(), parsed.calls.size() == 1 ? "" : "s", lat.totalMs / 1000.0);
        e.summary = b;
    }
    e.debugPrompt = "=== SYSTEM ===\n" + prompt.system + "\n\n=== USER (this tick) ===\n" + prompt.user
                    + "\n\n=== TOOLS ===\n12 fixed tool schemas (see report)";
    {
        const auto& msg = lastReply.contains ("choices") ? lastReply["choices"][0].value ("message", json::object()) : json::object();
        e.debugReply = "=== CONTENT ===\n" + (msg.contains ("content") && msg["content"].is_string() ? msg["content"].get<std::string>() : std::string())
                       + "\n\n=== TOOL CALLS ===\n" + (msg.contains ("tool_calls") ? msg["tool_calls"].dump (1) : std::string ("none"));
    }
    store.addChat (e);

    logs.latency (lat);
    logs.deltas (tick, val.deltas);
    ++completed;
    setStage (Stage::Done, (int) val.targets.size());
    st = State::Idle;
    return true;
}
} // namespace im
