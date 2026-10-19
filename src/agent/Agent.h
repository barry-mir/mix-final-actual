#pragma once
// Agent loop (spec §9). A worker thread runs ticks:
//  1. trigger: Auto interval (tick_s), user message, or stage drag end
//     (debounced by the caller). While a tick runs, at most one trigger is
//     queued (priority user > stage > auto; queued user messages are merged).
//  2. build prompt; hand the request to the bridge (the user's coding agent answers it).
//  3. parse tool calls (one retry on malformed JSON, then skip), validate,
//     clamp, max-step limits, skip locked; apply via the host (message thread,
//     LLM ramp).
//  4. append memory record; post rationale + applied diff to chat.
//  5. the next tick fills features_after of the previous record.
#include "agent/AgentLog.h"
#include "agent/Bridge.h"
#include "agent/PromptBuilder.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>

namespace im
{
class AgentHost
{
public:
    virtual ~AgentHost() = default;
    virtual TrackMask presentMask() const = 0;
    virtual float paramValue (int index) const = 0; // current target display value
    virtual std::shared_ptr<const Snapshot> snapshot() const = 0;
    virtual float groupBusGrDb (int /*group*/) const { return 0.0f; } // instrument-group bus compressor, max gain reduction
    virtual float groupBusGainDb (int /*group*/) const { return 0.0f; } // group bus output vs input level, dB
    // Thread-safe: called from the worker; must hop to the message thread itself.
    virtual void applyAgentChanges (int tick, const std::vector<ParamTarget>& targets, double rampSec) = 0;
    // Before measuring (worker thread): make the measurements describe the mix as it is now. Live engines render the
    // analysis range offline (no sound); offline engines play their own audio and need nothing. stop() = give up.
    virtual void listen (const std::function<bool()>& /*stop*/) {}
};

struct Trigger
{
    std::string kind = "auto"; // "auto" | "user" | "stage"
    std::string msg;
    std::string label; // what the coding agent sees as the turn's trigger (default: kind), e.g. "first_mix"
};

class Agent : private juce::Thread
{
public:
    enum class State { Offline, Idle, Thinking };

    Agent (StateStore& store, AgentHost& host, Bridge& bridge);
    ~Agent() override;

    void start();
    void stop();

    // Any thread. Ignored when mode is Off (user messages are still logged).
    void trigger (Trigger t);
    void userMessage (const std::string& text);
    // Run n turns one after another (the multi-turn method), each labelled `label` for the coding agent. A non-empty
    // goal is the user's aim for all of them (shown to the agent in every turn).
    void queueTurns (int n, const std::string& label, const std::string& goal = {});
    // Turns still queued after the one running (a request's follow-up checks, the rest of a remix).
    int turnsQueued() const { std::lock_guard<std::mutex> lk (m); return queued + (pending ? 1 : 0); }
    // A request typed by the user: one turn that answers it, then this many follow-up turns that measure the result
    // and finish the job (or change nothing when it is done).
    static constexpr int kRequestFollowUps = 2;

    // Where the current (or last) turn is, for the page. Reading (the coding agent has fetched the turn) is derived
    // from the bridge by the caller.
    enum class Stage { None, Listening, Waiting, Applying, Done };
    struct TurnStatus
    {
        int tick = -1;
        Stage stage = Stage::None;
        double sinceSec = 0; // in this stage
        int changes = 0;
    };
    TurnStatus turnStatus() const;
    static const char* stageName (Stage s);
    // Set by the app: true when a coding agent can be started automatically for a waiting turn (then the "waiting for
    // your coding agent" chat line is left out; the page shows the agent starting).
    std::function<bool()> autoAgentAvailable;

    State state() const noexcept { return st.load(); }
    static const char* stateName (State s);
    double thinkingElapsedSec() const;
    int ticksCompleted() const noexcept { return completed.load(); }
    AgentLog& log() noexcept { return logs; }

    // Synchronous single tick (tests / offline). Returns false if skipped.
    bool runTick (const Trigger& t);

    // How long a turn waits for the coding agent before it is skipped.
    void setTurnTimeout (double seconds) { turnTimeoutS = seconds; }

    // The user pressed Stop: drop queued turns (a remix's remaining turns, a waiting request). The caller also
    // aborts the turn waiting in the bridge, if any.
    void cancelQueued()
    {
        std::lock_guard<std::mutex> lk (m);
        pending.reset();
        queued = 0;
        stopRequested = true; // ends the turn in progress if it is still listening
    }

private:
    void run() override;
    std::optional<Trigger> takePending();

    StateStore& store;
    AgentHost& host;
    Bridge& bridge;
    AgentLog logs;

    mutable std::mutex m;
    std::optional<Trigger> pending;
    int queued = 0;            // turns still to run from queueTurns (guarded by m)
    std::atomic<bool> stopRequested { false };
    std::string queuedLabel, queuedGoal;
    void setStage (Stage s, int changes = 0);
    std::atomic<int> stageTick { -1 }, stageNow { 0 }, stageChanges { 0 };
    std::atomic<double> stageSinceMs { 0 };
    std::atomic<double> turnTimeoutS { 600.0 };
    std::atomic<State> st { State::Idle };
    std::atomic<double> thinkingSince { 0 };
    std::atomic<int> completed { 0 };
    double lastTickEndMs = 0;
    juce::WaitableEvent wake;
};
} // namespace im
