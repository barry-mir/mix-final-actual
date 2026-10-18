#include "agent/Agent.h"
#include <cstdlib>
#include <cmath>

namespace im
{
namespace
{
int priority (const std::string& k) { return k == "user" ? 3 : k == "stage" ? 2 : 1; }
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
} // namespace

Agent::Agent (StateStore& s, AgentHost& h, ServerManager* srv) : juce::Thread ("IM Agent"), store (s), host (h), server (srv)
{
    logs.setDir (ServerManager::logsDir());
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
    cancelToken.reset();
    lastTickEndMs = nowMs();
    if (! isThreadRunning()) startThread (juce::Thread::Priority::normal);
}

void Agent::stop()
{
    if (isThreadRunning())
    {
        signalThreadShouldExit();
        cancelToken.cancel(); // abort an in-flight request instead of waiting for its timeout
        wake.signal();
        stopThread (5000);
    }
}

void Agent::setEndpoint (const std::string& h, int p)
{
    std::lock_guard<std::mutex> lk (m);
    endpointHost = h;
    endpointPort = p;
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
        else if (priority (t.kind) >= priority (pending->kind))
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
}

std::optional<Trigger> Agent::takePending()
{
    std::lock_guard<std::mutex> lk (m);
    auto p = pending;
    pending.reset();
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

        if (server != nullptr && st.load() != State::Thinking)
            st = server->status() == ServerManager::Status::Online ? State::Idle : State::Offline;

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
    std::string h;
    int port;
    {
        std::lock_guard<std::mutex> lk (m);
        h = endpointHost;
        port = endpointPort > 0 ? endpointPort : cfg.port;
    }
    LlmClient client (h, port);
    const double t0 = nowMs();
    LatencyRow lat;
    lat.trigger = trig.kind;

    if (! client.health (800))
    {
        st = State::Offline;
        ChatEntry s;
        s.role = "system";
        s.text = "LLM offline (no llama-server on port " + std::to_string (port) + "). Mixer keeps working.";
        s.time = juce::Time::currentTimeMillis() / 1000.0;
        store.addChat (s);
        return false;
    }

    st = State::Thinking;
    thinkingSince = nowMs();
    const int tick = store.nextTickNumber();
    lat.tick = tick;

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
    // Exact counts from the server's tokenizer when available (fixed prefix measured once per endpoint).
    {
        const std::string key = h + ":" + std::to_string (port);
        if (fixedKey != key)
        {
            // Exact fixed prefix: system + tools rendered by the server's own chat template (empty user turn).
            fixedTokensCached = client.templatedTokens (json::array ({ { { "role", "system" }, { "content", systemPrompt (cfg.promptVersion) } },
                                                                      { { "role", "user" }, { "content", "" } } }));
            if (fixedTokensCached < 0) // older servers without /apply-template
            {
                const int sys = client.tokenize (systemPrompt()), tools = client.tokenize (toolSchemas().dump (1));
                fixedTokensCached = (sys >= 0 && tools >= 0) ? sys + tools + kTemplateOverheadTokens : -1;
            }
            fixedKey = key;
        }
        if (fixedTokensCached >= 0)
        {
            in.fixedTokens = fixedTokensCached;
            in.countTokens = [&client] (const std::string& text) { return client.tokenize (text); };
        }
    }
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
        const auto request = LlmClient::makeRequest (messages, cfg.maxTokens, cfg.enableThinking);
        static const int timeoutS = [] { const char* e = std::getenv ("IM_LLM_TIMEOUT_S"); return e ? std::max (30, std::atoi (e)) : 300; }();
        auto res = client.chat (request, timeoutS, &cancelToken);
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
        s.text = "Tick " + std::to_string (tick) + " skipped: " + failure.substr (0, 200);
        s.time = rec.time;
        store.addChat (s);
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
    ctx.userRequest = trig.kind == "user";
    const auto val = validateToolCalls (parsed.calls, ctx);

    // apply (message thread, LLM ramp)
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
        int nTracks = 0;
        for (int t = 0; t < kMaxTracks; ++t) nTracks += (int) ((mask >> t) & 1u);
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
    st = State::Idle;
    return true;
}
} // namespace im
