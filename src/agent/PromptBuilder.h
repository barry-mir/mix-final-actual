#pragma once
// Prompt layout (spec §9.1):
//   [system]  fixed: role, conventions (units, feature legend, stage coords), rules   } fixed prefix
//   [tools]   fixed schemas (request "tools")                                          } -> KV cache reuse
//   [user]    TRACKS (session context: index, name, stage pos + current features),
//             LOCKED, MEMORY (last K records), SETTINGS (non-default params), TRIGGER
// Compact text tables, rounded (dB 0.1, Hz integer, ratios 0.1). If the total
// exceeds the token budget the oldest memory records are dropped first.
#include "agent/Tools.h"
#include "analysis/Analyzer.h"

namespace im
{
struct PromptInputs
{
    std::vector<std::string> names = std::vector<std::string> ((size_t) maxTracks());
    std::vector<StagePos> stage = std::vector<StagePos> ((size_t) maxTracks());
    TrackMask presentMask {};
    std::vector<int> locked;
    std::vector<MemoryRecord> memory;           // last K, oldest first
    std::function<float (int)> paramValue;      // display value
    std::function<float (int)> groupBusGr;      // instrument-group bus gain reduction, dB (may be empty)
    std::function<float (int)> groupBusGain;    // instrument-group bus output vs input level, dB (may be empty)
    std::shared_ptr<const Snapshot> snapshot;
    std::string trigger;                        // "auto" | "user" | "stage"
    std::string userMsg;
    int tokenBudget = 3500;
    // Optional exact token counting (llama-server /tokenize); falls back to estimateTokens.
    std::function<int (const std::string&)> countTokens;
    int fixedTokens = -1;                       // system + tools + template; -1 = estimate
    int promptVersion = 3;                      // 1 = tables only, 2 = tables + diagnoses vs priors, 3 = + checklist and finished-mix reference
};

struct BuiltPrompt
{
    json messages;          // [system, user]
    std::string system, user;
    int estTokens = 0;      // system + tools + template + user
    int userTokens = 0, fixedTokens = 0;
    bool exactCount = false;
    int memoryUsed = 0, memoryDropped = 0;
};

const std::string& systemPrompt (int version = 2);
// Structural estimate tuned on the Qwen tokenizer: digits are single tokens,
// letter runs ~5 chars/token, punctuation 1, whitespace runs (except single spaces) 1.
int estimateTokens (const std::string& s);
int fixedPrefixEstimate (int version = 2); // system + tools + chat-template overhead
constexpr int kTemplateOverheadTokens = 225; // Qwen3.5 chat template around tools + messages (measured)
BuiltPrompt buildPrompt (const PromptInputs& in);

// Tone deviations of a track from its role's typical profile (at most 2, strongest first).
// Track level relative to the mix as the agent reads it: the long-term average since the last parameter change when it
// covers at least 6 s of audio, else the latest 3 s window.
float levelVsMix (const Snapshot& s, int track);

struct ToneIssue
{
    int band;           // analysis band 0..7
    float dev;          // dB (positive = too strong)
    int eqSlot;         // EQ band param slot for the gain (LfGain/LmfGain/HmfGain/HfGain)
    float eqFreq;       // suggested centre/corner Hz
};
std::vector<ToneIssue> trackToneIssues (const StereoFeatures& post, int role);

// Compact feature JSON for memory records: master + the given tracks (0-based).
json compactFeatures (const Snapshot* s, const std::vector<int>& tracks);
std::vector<int> tracksTouched (const std::vector<AppliedChange>& changes);

// Individual sections (exposed for tests/demo).
std::string tracksSection (const PromptInputs& in);
std::string tracksSectionV2 (const PromptInputs& in); // with level/tone diagnoses
std::string tracksSectionV3 (const PromptInputs& in); // plain table + CHECKLIST of fixes computed against finished mixes
std::vector<std::string> checklistV3 (const PromptInputs& in);
std::string tracksSectionV4 (const PromptInputs& in); // engineer prompt: table, band shares, technical warnings only
std::string lockedSection (const PromptInputs& in);
std::string memorySection (const std::vector<MemoryRecord>& mem, const std::vector<std::string>& names);
std::string settingsSection (const PromptInputs& in);
std::string triggerSection (const PromptInputs& in);
} // namespace im
