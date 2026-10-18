#include "agent/AgentLog.h"

namespace im
{
const char* AgentLog::latencyHeader()
{
    return "tick,unix_time,trigger,status,prefill_ms,decode_ms,total_ms,prompt_tokens,completion_tokens,est_prompt_tokens,retries,num_calls";
}

const char* AgentLog::deltasHeader()
{
    return "tick,unix_time,tool,param,status,current,requested,applied,requested_delta,applied_delta,note";
}

void AgentLog::setDir (const juce::File& d)
{
    std::lock_guard<std::mutex> lk (m);
    logDir = d;
    logDir.createDirectory();
}

juce::File AgentLog::dir() const
{
    std::lock_guard<std::mutex> lk (m);
    return logDir;
}

void AgentLog::append (const juce::File& f, const char* header, const std::string& line)
{
    if (! f.existsAsFile()) f.appendText (juce::String (header) + "\n", false, false, "\n");
    f.appendText (juce::String (line) + "\n", false, false, "\n");
}

static std::string csvEscape (std::string s)
{
    bool q = s.find_first_of (",\"\n") != std::string::npos;
    std::string o;
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return q ? "\"" + o + "\"" : o;
}

void AgentLog::latency (const LatencyRow& r)
{
    std::lock_guard<std::mutex> lk (m);
    if (logDir == juce::File()) return;
    char b[512];
    std::snprintf (b, sizeof (b), "%d,%.3f,%s,%s,%.1f,%.1f,%.1f,%d,%d,%d,%d,%d", r.tick,
                   juce::Time::currentTimeMillis() / 1000.0, r.trigger.c_str(), r.status.c_str(), r.prefillMs, r.decodeMs,
                   r.totalMs, r.promptTokens, r.completionTokens, r.estPromptTokens, r.retries, r.numCalls);
    append (logDir.getChildFile ("agent_latency.csv"), latencyHeader(), b);
}

void AgentLog::deltas (int tick, const std::vector<DeltaLog>& d)
{
    std::lock_guard<std::mutex> lk (m);
    if (logDir == juce::File()) return;
    const double now = juce::Time::currentTimeMillis() / 1000.0;
    for (auto& x : d)
    {
        char b[512];
        std::snprintf (b, sizeof (b), "%d,%.3f,%s,%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f,", tick, now, x.tool.c_str(), x.param.c_str(),
                       x.status.c_str(), x.current, x.requested, x.applied, x.requestedDelta, x.appliedDelta);
        append (logDir.getChildFile ("agent_deltas.csv"), deltasHeader(), std::string (b) + csvEscape (x.note));
    }
}
} // namespace im

namespace im
{
void AgentLog::transcript (int tick, int attempt, const nlohmann::json& request, const nlohmann::json& response)
{
    if (! transcriptOn.load()) return;
    std::lock_guard<std::mutex> lk (m);
    if (logDir == juce::File()) return;
    nlohmann::json j = { { "tick", tick }, { "attempt", attempt }, { "unix_time", juce::Time::currentTimeMillis() / 1000.0 },
                         { "request", request }, { "response", response } };
    logDir.getChildFile ("agent_transcript.jsonl").appendText (juce::String (j.dump()) + "\n", false, false, "\n");
}
} // namespace im
