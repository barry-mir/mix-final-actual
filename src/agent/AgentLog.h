#pragma once
// CSV logs: logs/agent_latency.csv (per tick) and logs/agent_deltas.csv
// (requested vs applied delta for every parameter of every tool call).
#include "agent/Tools.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdlib>
#include <mutex>

namespace im
{
struct LatencyRow
{
    int tick = 0;
    std::string trigger, status;
    double prefillMs = 0, decodeMs = 0, totalMs = 0;
    int promptTokens = 0, completionTokens = 0, estPromptTokens = 0, retries = 0, numCalls = 0;
};

class AgentLog
{
public:
    void setDir (const juce::File& d);
    juce::File dir() const;
    void latency (const LatencyRow& r);
    void deltas (int tick, const std::vector<DeltaLog>& d);
    // Optional raw transcript (logs/agent_transcript.jsonl): request + response per LLM call.
    void setTranscriptEnabled (bool e) { transcriptOn = e; }
    void transcript (int tick, int attempt, const nlohmann::json& request, const nlohmann::json& response);

    static const char* latencyHeader();
    static const char* deltasHeader();

private:
    void append (const juce::File& f, const char* header, const std::string& line);
    mutable std::mutex m;
    juce::File logDir;
    std::atomic<bool> transcriptOn { std::getenv ("IM_LOG_TRANSCRIPT") != nullptr };
};
} // namespace im
