#pragma once
// Agent tools (LLM2Fx-Tools style structured JSON tool calls, Doh et al. 2026):
// OpenAI-format schemas, response parsing (incl. fallbacks for tool calls
// embedded in content), and validation: track resolution, range clamping,
// LLM locks, mute-only-on-user-request, optional max-step limits.
#include "core/StateStore.h"
#include <functional>

namespace im
{
const json& toolSchemas(); // fixed array, part of the cacheable prompt prefix
// IM_PAPER_DYNAMICS=1: show the model the paper's dynamics tools and readings (no group buses, drive as saturation,
// no whole-excerpt crest). The DSP and its fixes are unchanged; only what the model is told and offered.
bool paperDynamics();

struct ToolCall
{
    std::string id, name;
    json args;              // parsed arguments
    std::string rawArgs;    // as received
};

struct ParsedResponse
{
    std::string content;            // rationale (think-blocks and inline tool-call markup removed)
    std::vector<ToolCall> calls;
    std::string parseError;         // non-empty -> malformed (retry once)
    std::string finishReason;
    int promptTokens = 0, completionTokens = 0;
    double promptMs = 0, predictedMs = 0;
};

ParsedResponse parseChatResponse (const json& response);

struct ParamTarget
{
    int param = -1;
    float value = 0;
};

struct ToolContext
{
    std::array<std::string, kMaxTracks> names;
    TrackMask presentMask = 0xFFFF;
    std::function<float (int)> currentValue; // display value
    std::function<bool (int)> isLocked;
    Settings settings;
    bool userRequest = false; // tick triggered by a user message (allows set_mute)
};

struct DeltaLog
{
    std::string tool, param, status, note;
    float current = 0, requested = 0, applied = 0;
    double requestedDelta = 0, appliedDelta = 0;
};

struct ValidationResult
{
    std::vector<ParamTarget> targets;       // to apply (deduplicated, last wins)
    std::vector<AppliedChange> changes;     // everything incl. rejected, for record/chat
    std::vector<DeltaLog> deltas;           // for logs/agent_deltas.csv
    std::vector<std::string> errors;        // call-level problems (unknown tool, bad track)
    bool noChange = false;
    std::string noChangeReason;
};

ValidationResult validateToolCalls (const std::vector<ToolCall>& calls, const ToolContext& ctx);

// Resolve "track" argument: integer index (1-based) or exact name. Returns 0-based or -1.
int resolveTrack (const json& v, const std::array<std::string, kMaxTracks>& names);
} // namespace im
