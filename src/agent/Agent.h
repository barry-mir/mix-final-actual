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
    // Run n turns one after another (the multi-turn method), each labelled `label` for the coding agent.
    void queueTurns (int n, const std::string& label);

    State state() const noexcept { return st.load(); }
    static const char* stateName (State s);
    double thinkingElapsedSec() const;
    int ticksCompleted() const noexcept { return completed.load(); }
    AgentLog& log() noexcept { return logs; }

    // Synchronous single tick (tests / offline). Returns false if skipped.
    bool runTick (const Trigger& t);

    // How long a turn waits for the coding agent before it is skipped.
    void setTurnTimeout (double seconds) { turnTimeoutS = seconds; }

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
    std::string queuedLabel;
    std::atomic<double> turnTimeoutS { 1800.0 };
    std::atomic<State> st { State::Idle };
    std::atomic<double> thinkingSince { 0 };
    std::atomic<int> completed { 0 };
    double lastTickEndMs = 0;
    juce::WaitableEvent wake;
};
} // namespace im
