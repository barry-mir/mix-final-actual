#pragma once
// Agent loop (spec §9). A worker thread runs ticks:
//  1. trigger: Auto interval (tick_s), user message, or stage drag end
//     (debounced by the caller). While a tick runs, at most one trigger is
//     queued (priority user > stage > auto; queued user messages are merged).
//  2. build prompt; send request on the worker thread.
//  3. parse tool calls (one retry on malformed JSON, then skip), validate,
//     clamp, max-step limits, skip locked; apply via the host (message thread,
//     LLM ramp).
//  4. append memory record; post rationale + applied diff to chat.
//  5. the next tick fills features_after of the previous record.
#include "agent/AgentLog.h"
#include "agent/LlmClient.h"
#include "agent/PromptBuilder.h"
#include "agent/ServerManager.h"
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
};

class Agent : private juce::Thread
{
public:
    enum class State { Offline, Idle, Thinking };

    Agent (StateStore& store, AgentHost& host, ServerManager* server);
    ~Agent() override;

    void start();
    void stop();

    // Any thread. Ignored when mode is Off (user messages are still logged).
    void trigger (Trigger t);
    void userMessage (const std::string& text);

    State state() const noexcept { return st.load(); }
    static const char* stateName (State s);
    double thinkingElapsedSec() const;
    int ticksCompleted() const noexcept { return completed.load(); }
    AgentLog& log() noexcept { return logs; }

    // Synchronous single tick (tests / offline). Returns false if skipped.
    bool runTick (const Trigger& t);

    // Host/port override (tests use a mock server).
    void setEndpoint (const std::string& host, int port);

private:
    void run() override;
    std::optional<Trigger> takePending();

    StateStore& store;
    AgentHost& host;
    ServerManager* server;
    AgentLog logs;

    mutable std::mutex m;
    std::optional<Trigger> pending;
    std::string endpointHost = "127.0.0.1";
    int endpointPort = -1; // -1 = from settings
    std::atomic<State> st { State::Idle };
    std::atomic<double> thinkingSince { 0 };
    std::atomic<int> completed { 0 };
    double lastTickEndMs = 0;
    std::string fixedKey;
    int fixedTokensCached = -1;
    juce::WaitableEvent wake;
    CancelToken cancelToken; // aborts an in-flight request on stop()
};
} // namespace im
