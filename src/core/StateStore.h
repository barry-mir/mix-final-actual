#pragma once
// UI-independent, non-APVTS plugin state: track names, stage positions,
// per-param LLM locks, agent memory records, chat log and settings.
// Thread-safe (message thread + agent worker); lock flags are lock-free.
#include "core/Params.h"
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace im
{
using json = nlohmann::json;

enum class AgentMode { Off, Assist, Auto };
const char* modeName (AgentMode m);
AgentMode modeFromName (const std::string& s);

// Max-change-per-tick table keys (value or none). Off by default (all none).
const std::vector<std::string>& maxStepKeys();
// Which max-step key applies to a param ("" = none, e.g. discrete params).
std::string maxStepKeyFor (const ParamSpec& s);
// Delta between two display values in the key's unit (dB, octaves, ratio...).
double stepDelta (const ParamSpec& s, double from, double to);

struct Settings
{
    std::string modelPath;
    std::string serverPath = "llama-server";
    int port = 8089;
    bool autoStart = true;
    bool cpuOnly = false;    // false = all layers on the GPU (Metal, -ngl 99; ~3x faster per turn); true = -ngl 0 (the spec's CPU-only agent)
    double tickS = 30.0;     // Auto interval, 2..300 (30 s: CPU-only ticks take 23-31 s)
    int memoryK = 3;         // records in prompt (3: fits the token budget with 5 busy tracks)
    double llmRampS = 2.0;   // 0.5..5
    std::map<std::string, std::optional<double>> maxStep; // key -> limit or none
    AgentMode mode = AgentMode::Off;
    int promptTokenBudget = 6000; // raised from 3500 (user OK): v2 diagnoses + memory without dropping
    int promptVersion = 3;      // 1 = original tables, 2 = tables + diagnoses vs. mix priors, 3 = + checklist vs. 230 finished mixes, 4 = engineer, 5 = engineer for many tracks + full effect set (API models)
    int maxTokens = 1000;       // completion budget per tick (400 truncated multi-call replies)
    bool enableThinking = false;
    int contextSize = 16384;    // llama-server -c (raised from 8192)

    json toJson() const;
    static Settings fromJson (const json& j);
    void clampAll();
};

enum class NameSource { Default, File, Host, User }; // host/file names never overwrite a user-typed name

struct StagePos
{
    float x = 0, y = 0.2f;
};

struct AppliedChange
{
    std::string param;     // id
    float from = 0, to = 0;
    float requested = 0;   // value requested by the LLM before clamps/limits
    std::string status;    // "applied" | "clamped" | "limited" | "rejected"
    std::string note;
    json toJson() const;
    static AppliedChange fromJson (const json& j);
};

struct MemoryRecord
{
    int tick = 0;
    double time = 0;            // unix seconds
    std::string trigger;        // "auto" | "user" | "stage"
    std::string userMsg;        // may be empty
    json featuresBefore;        // compact: master + tracks changed in this tick
    std::string rationale;
    std::vector<AppliedChange> applied;
    std::vector<json> toolCalls;  // raw tool calls as returned
    json featuresAfter;         // filled by the next tick (null until then)
    double latencyPrefillMs = 0, latencyDecodeMs = 0, latencyTotalMs = 0;
    json toJson() const;
    static MemoryRecord fromJson (const json& j);
};

struct ChatEntry
{
    std::string role;   // "user" | "agent" | "system"
    std::string text;
    int tick = -1;
    double time = 0;
    std::vector<AppliedChange> diff;
    bool undone = false;
    std::string summary;                 // "read 5 tracks, 3 memory turns, 2,850 tokens, 24 s"
    std::string debugPrompt, debugReply; // transient (not saved): exactly what the model read and answered
    json toJson() const;
    static ChatEntry fromJson (const json& j);
};

class StateStore
{
public:
    StateStore();

    // Track names / stage
    std::string trackName (int t) const;
    void setTrackName (int t, const std::string& n, NameSource src = NameSource::User);
    NameSource nameSource (int t) const;
    std::array<std::string, kMaxTracks> trackNames() const;
    StagePos stagePos (int t) const;
    void setStagePos (int t, StagePos p);
    std::array<StagePos, kMaxTracks> stagePositions() const;
    static StagePos defaultStagePos (int t, int numTracks);
    void resetStage (int numTracks);

    // Locks
    bool isLocked (int param) const noexcept { return locks[(size_t) param].load (std::memory_order_relaxed); }
    void setLocked (int param, bool l);
    std::vector<int> lockedParams() const;

    // Settings
    Settings settings() const;
    void setSettings (const Settings& s);

    // Memory
    int nextTickNumber();
    void addRecord (MemoryRecord r);
    void setFeaturesAfterForLast (const json& f);
    std::vector<MemoryRecord> records() const;   // all (export)
    std::vector<MemoryRecord> lastRecords (int k) const;

    // Chat
    void addChat (ChatEntry e);
    std::vector<ChatEntry> chat() const;
    void markTickUndone (int tick);
    uint64_t chatVersion() const noexcept { return chatVer.load(); }

    json toJson() const;
    void fromJson (const json& j);

private:
    mutable std::mutex m;
    std::array<std::string, kMaxTracks> names;
    std::array<NameSource, kMaxTracks> nameSrc {};
    std::array<StagePos, kMaxTracks> stage;
    std::array<std::atomic<bool>, kNumParams> locks {};
    Settings cfg;
    std::deque<MemoryRecord> recs; // full history kept for export (bounded)
    std::vector<ChatEntry> chatLog;
    int tickCounter = 0;
    std::atomic<uint64_t> chatVer { 0 };
    std::atomic<uint64_t> nameVer { 0 };
public:
    uint64_t namesVersion() const noexcept { return nameVer.load(); }
};
} // namespace im
