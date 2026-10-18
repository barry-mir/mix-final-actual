#include "core/StateStore.h"
#include <algorithm>
#include <cmath>

namespace im
{
const char* modeName (AgentMode m)
{
    switch (m)
    {
        case AgentMode::Assist: return "Assist";
        case AgentMode::Auto: return "Auto";
        default: return "Off";
    }
}

AgentMode modeFromName (const std::string& s)
{
    if (s == "Assist") return AgentMode::Assist;
    if (s == "Auto") return AgentMode::Auto;
    return AgentMode::Off;
}

const std::vector<std::string>& maxStepKeys()
{
    static const std::vector<std::string> k = {
        "fader_db", "trim_db", "send_db", "balance", "width", "eq_gain_db", "eq_freq_oct", "eq_q",
        "filter_freq_oct", "threshold_db", "comp_ratio", "makeup_db", "time_ms", "range_db",
        "reverb_decay_s", "reverb_size", "reverb_freq_oct", "reverb_predelay_ms",
    };
    return k;
}

std::string maxStepKeyFor (const ParamSpec& s)
{
    if (s.kind != Kind::Float) return "";
    const auto& k = s.key;
    if (k == "fader.gain") return "fader_db";
    if (k == "trim.gain") return "trim_db";
    if (k == "send.gain" || k == "reverb.return" || k == "reverb.er_level") return "send_db";
    if (k == "balance.value") return "balance";
    if (k == "width.amount" || k == "reverb.width") return "width";
    if (s.module == "eq" && s.name.find ("_gain") != std::string::npos) return "eq_gain_db";
    if (s.module == "eq" && s.name.find ("_freq") != std::string::npos) return "eq_freq_oct";
    if (s.module == "eq" && s.name.find ("_q") != std::string::npos) return "eq_q";
    if (s.module == "hpf" || s.module == "lpf") return "filter_freq_oct";
    if (s.name == "threshold") return "threshold_db";
    if (s.name == "ratio") return "comp_ratio";
    if (s.name == "makeup" || s.name == "knee") return "makeup_db";
    if (s.name == "range") return "range_db";
    if (s.unit == "ms" && s.module != "reverb") return "time_ms";
    if (k == "reverb.decay") return "reverb_decay_s";
    if (k == "reverb.size") return "reverb_size";
    if (k == "reverb.predelay") return "reverb_predelay_ms";
    if (s.module == "reverb" && s.unit == "Hz") return "reverb_freq_oct";
    if (k == "fader.gain") return "fader_db";
    return "";
}

double stepDelta (const ParamSpec& s, double from, double to)
{
    if (s.domain == Domain::LogHz) return std::log2 (std::max (to, 1.0) / std::max (from, 1.0));
    return to - from;
}

// ---------------------------------------------------------------- Settings
json Settings::toJson() const
{
    json ms = json::object();
    for (auto& [k, v] : maxStep) ms[k] = v ? json (*v) : json ("none");
    return { { "tick_s", tickS }, { "K", memoryK }, { "llm_ramp_s", llmRampS }, { "max_step", ms },
             { "mode", modeName (mode) }, { "prompt_token_budget", promptTokenBudget },
             { "prompt_version", promptVersion }, { "settings_rev", 4 }, { "max_tokens", maxTokens }, { "enable_thinking", enableThinking } };
}

Settings Settings::fromJson (const json& j)
{
    Settings s;
    s.tickS = j.value ("tick_s", s.tickS);
    s.memoryK = j.value ("K", s.memoryK);
    s.llmRampS = j.value ("llm_ramp_s", s.llmRampS);
    s.mode = modeFromName (j.value ("mode", std::string ("Off")));
    s.promptTokenBudget = j.value ("prompt_token_budget", s.promptTokenBudget);
    s.promptVersion = j.value ("prompt_version", s.promptVersion);
    if (j.value ("settings_rev", 0) < 3 && s.promptVersion == 2) s.promptVersion = 3; // v2 was the default before the checklist prompt
    s.maxTokens = j.value ("max_tokens", s.maxTokens);
    s.enableThinking = j.value ("enable_thinking", s.enableThinking);
    if (! j.contains ("prompt_version")) // settings saved before v2: lift the old 3500-token budget / 8k context
    {
        s.promptTokenBudget = std::max (s.promptTokenBudget, Settings().promptTokenBudget);
        s.maxStep["eq_gain_db"] = 6.0; // new default limit (evaluation: unlimited EQ moves of up to 18 dB were harmful)
    }
    for (auto& k : maxStepKeys()) s.maxStep[k] = k == "eq_gain_db" ? std::optional<double> (6.0) : std::nullopt;
    if (j.contains ("max_step") && j["max_step"].is_object())
        for (auto& [k, v] : j["max_step"].items())
            s.maxStep[k] = v.is_number() ? std::optional<double> (v.get<double>()) : std::nullopt;
    s.clampAll();
    return s;
}

void Settings::clampAll()
{
    tickS = std::clamp (tickS, 2.0, 300.0);
    memoryK = std::clamp (memoryK, 0, 50);
    llmRampS = std::clamp (llmRampS, 0.5, 5.0);
    promptVersion = std::clamp (promptVersion, 1, 5);
    maxTokens = std::clamp (maxTokens, 64, 8192);
    promptTokenBudget = std::clamp (promptTokenBudget, 1000, 1 << 20);
    for (auto& k : maxStepKeys())
        if (! maxStep.count (k)) maxStep[k] = k == "eq_gain_db" ? std::optional<double> (6.0) : std::nullopt; // EQ: at most 6 dB per tick by default
}

// ---------------------------------------------------------------- records
json AppliedChange::toJson() const
{
    json j = { { "param", param }, { "from", from }, { "to", to }, { "requested", requested }, { "status", status } };
    if (! note.empty()) j["note"] = note;
    return j;
}

AppliedChange AppliedChange::fromJson (const json& j)
{
    AppliedChange c;
    c.param = j.value ("param", "");
    c.from = j.value ("from", 0.0f);
    c.to = j.value ("to", 0.0f);
    c.requested = j.value ("requested", 0.0f);
    c.status = j.value ("status", "");
    c.note = j.value ("note", "");
    return c;
}

json MemoryRecord::toJson() const
{
    json ap = json::array();
    for (auto& a : applied) ap.push_back (a.toJson());
    return { { "tick", tick }, { "time", time }, { "trigger", trigger }, { "user_msg", userMsg },
             { "features_before", featuresBefore }, { "rationale", rationale }, { "applied_changes", ap },
             { "tool_calls", toolCalls }, { "features_after", featuresAfter },
             { "latency_ms", { { "prefill", latencyPrefillMs }, { "decode", latencyDecodeMs }, { "total", latencyTotalMs } } } };
}

MemoryRecord MemoryRecord::fromJson (const json& j)
{
    MemoryRecord r;
    r.tick = j.value ("tick", 0);
    r.time = j.value ("time", 0.0);
    r.trigger = j.value ("trigger", "");
    r.userMsg = j.value ("user_msg", "");
    r.featuresBefore = j.value ("features_before", json());
    r.rationale = j.value ("rationale", "");
    if (j.contains ("applied_changes"))
        for (auto& a : j["applied_changes"]) r.applied.push_back (AppliedChange::fromJson (a));
    if (j.contains ("tool_calls") && j["tool_calls"].is_array())
        for (auto& t : j["tool_calls"]) r.toolCalls.push_back (t);
    r.featuresAfter = j.value ("features_after", json());
    if (j.contains ("latency_ms"))
    {
        r.latencyPrefillMs = j["latency_ms"].value ("prefill", 0.0);
        r.latencyDecodeMs = j["latency_ms"].value ("decode", 0.0);
        r.latencyTotalMs = j["latency_ms"].value ("total", 0.0);
    }
    return r;
}

json ChatEntry::toJson() const
{
    json d = json::array();
    for (auto& a : diff) d.push_back (a.toJson());
    return { { "role", role }, { "text", text }, { "tick", tick }, { "time", time }, { "diff", d }, { "undone", undone }, { "summary", summary } };
}

ChatEntry ChatEntry::fromJson (const json& j)
{
    ChatEntry e;
    e.role = j.value ("role", "");
    e.text = j.value ("text", "");
    e.tick = j.value ("tick", -1);
    e.time = j.value ("time", 0.0);
    e.undone = j.value ("undone", false);
    e.summary = j.value ("summary", "");
    if (j.contains ("diff"))
        for (auto& a : j["diff"]) e.diff.push_back (AppliedChange::fromJson (a));
    return e;
}

// ---------------------------------------------------------------- store
StateStore::StateStore()
{
    for (int t = 0; t < kMaxTracks; ++t) names[(size_t) t] = "Track " + std::to_string (t + 1);
    resetStage (kMaxTracks);
    cfg.clampAll();
}

std::string StateStore::trackName (int t) const
{
    std::lock_guard<std::mutex> lk (m);
    return names[(size_t) t];
}

void StateStore::setTrackName (int t, const std::string& n, NameSource src)
{
    std::lock_guard<std::mutex> lk (m);
    names[(size_t) t] = n.empty() ? "Track " + std::to_string (t + 1) : n;
    nameSrc[(size_t) t] = n.empty() ? NameSource::Default : src;
    ++nameVer;
}

NameSource StateStore::nameSource (int t) const
{
    std::lock_guard<std::mutex> lk (m);
    return nameSrc[(size_t) t];
}

std::array<std::string, kMaxTracks> StateStore::trackNames() const
{
    std::lock_guard<std::mutex> lk (m);
    return names;
}

StagePos StateStore::stagePos (int t) const
{
    std::lock_guard<std::mutex> lk (m);
    return stage[(size_t) t];
}

void StateStore::setStagePos (int t, StagePos p)
{
    std::lock_guard<std::mutex> lk (m);
    p.x = std::clamp (p.x, -1.0f, 1.0f);
    p.y = std::clamp (p.y, 0.0f, 1.0f);
    stage[(size_t) t] = p;
}

std::array<StagePos, kMaxTracks> StateStore::stagePositions() const
{
    std::lock_guard<std::mutex> lk (m);
    return stage;
}

StagePos StateStore::defaultStagePos (int t, int numTracks)
{
    // Spread across the front row.
    const int n = std::max (1, numTracks);
    const float x = n == 1 ? 0.0f : -0.9f + 1.8f * (float) t / (float) (n - 1);
    return { std::clamp (x, -1.0f, 1.0f), 0.1f };
}

void StateStore::resetStage (int numTracks)
{
    std::lock_guard<std::mutex> lk (m);
    for (int t = 0; t < kMaxTracks; ++t) stage[(size_t) t] = defaultStagePos (t, numTracks);
}

void StateStore::setLocked (int param, bool l) { locks[(size_t) param].store (l); }

std::vector<int> StateStore::lockedParams() const
{
    std::vector<int> v;
    for (int i = 0; i < kNumParams; ++i)
        if (locks[(size_t) i].load()) v.push_back (i);
    return v;
}

Settings StateStore::settings() const
{
    std::lock_guard<std::mutex> lk (m);
    return cfg;
}

void StateStore::setSettings (const Settings& s)
{
    std::lock_guard<std::mutex> lk (m);
    cfg = s;
    cfg.clampAll();
}

int StateStore::nextTickNumber()
{
    std::lock_guard<std::mutex> lk (m);
    return ++tickCounter;
}

void StateStore::addRecord (MemoryRecord r)
{
    std::lock_guard<std::mutex> lk (m);
    recs.push_back (std::move (r));
    while (recs.size() > 500) recs.pop_front();
}

void StateStore::setFeaturesAfterForLast (const json& f)
{
    std::lock_guard<std::mutex> lk (m);
    if (! recs.empty() && recs.back().featuresAfter.is_null()) recs.back().featuresAfter = f;
}

std::vector<MemoryRecord> StateStore::records() const
{
    std::lock_guard<std::mutex> lk (m);
    return { recs.begin(), recs.end() };
}

std::vector<MemoryRecord> StateStore::lastRecords (int k) const
{
    std::lock_guard<std::mutex> lk (m);
    const size_t n = std::min (recs.size(), (size_t) std::max (0, k));
    return { recs.end() - (long) n, recs.end() };
}

void StateStore::addChat (ChatEntry e)
{
    {
        std::lock_guard<std::mutex> lk (m);
        chatLog.push_back (std::move (e));
        if (chatLog.size() > 1000) chatLog.erase (chatLog.begin());
    }
    ++chatVer;
}

std::vector<ChatEntry> StateStore::chat() const
{
    std::lock_guard<std::mutex> lk (m);
    return chatLog;
}

void StateStore::markTickUndone (int tick)
{
    {
        std::lock_guard<std::mutex> lk (m);
        for (auto& c : chatLog)
            if (c.tick == tick && c.role == "agent") c.undone = true;
    }
    ++chatVer;
}

json StateStore::toJson() const
{
    std::lock_guard<std::mutex> lk (m);
    json j;
    j["version"] = 1;
    j["track_names"] = names;
    { json ns = json::array(); for (auto s : nameSrc) ns.push_back ((int) s); j["name_sources"] = ns; }
    json st = json::array();
    for (auto& p : stage) st.push_back ({ p.x, p.y });
    j["stage"] = st;
    json lk2 = json::array();
    for (int i = 0; i < kNumParams; ++i)
        if (locks[(size_t) i].load()) lk2.push_back (spec (i).id);
    j["llm_locks"] = lk2;
    j["settings"] = cfg.toJson();
    json r = json::array();
    for (auto& rec : recs) r.push_back (rec.toJson());
    j["memory"] = r;
    json c = json::array();
    for (auto& e : chatLog) c.push_back (e.toJson());
    j["chat"] = c;
    j["tick_counter"] = tickCounter;
    return j;
}

void StateStore::fromJson (const json& j)
{
    {
        std::lock_guard<std::mutex> lk (m);
        if (j.contains ("track_names"))
            for (size_t t = 0; t < (size_t) kMaxTracks && t < j["track_names"].size(); ++t)
            {
                names[t] = j["track_names"][t].get<std::string>();
                nameSrc[t] = j.contains ("name_sources") && t < j["name_sources"].size() ? (NameSource) j["name_sources"][t].get<int>()
                             : (names[t] == "Track " + std::to_string (t + 1) ? NameSource::Default : NameSource::User);
            }
        if (j.contains ("stage"))
            for (size_t t = 0; t < (size_t) kMaxTracks && t < j["stage"].size(); ++t)
                stage[t] = { j["stage"][t][0].get<float>(), j["stage"][t][1].get<float>() };
        if (j.contains ("settings")) cfg = Settings::fromJson (j["settings"]);
        recs.clear();
        if (j.contains ("memory"))
            for (auto& r : j["memory"]) recs.push_back (MemoryRecord::fromJson (r));
        chatLog.clear();
        if (j.contains ("chat"))
            for (auto& e : j["chat"]) chatLog.push_back (ChatEntry::fromJson (e));
        tickCounter = j.value ("tick_counter", 0);
    }
    for (auto& l : locks) l.store (false);
    if (j.contains ("llm_locks"))
        for (auto& id : j["llm_locks"])
        {
            const int i = findParam (id.get<std::string>());
            if (i >= 0) locks[(size_t) i].store (true);
        }
    ++chatVer;
}
} // namespace im
