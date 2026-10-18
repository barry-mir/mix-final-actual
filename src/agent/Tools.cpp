#include <cstdlib>
#include "agent/Tools.h"
#include <algorithm>
#include <cmath>
#include <regex>

namespace im
{
namespace
{
json num() { return { { "type", "number" } }; }
json boolean() { return { { "type", "boolean" } }; }
json str() { return { { "type", "string" } }; }
json enm (std::initializer_list<const char*> v)
{
    json a = json::array();
    for (auto* x : v) a.push_back (x);
    return { { "type", "string" }, { "enum", a } };
}

json fn (const char* name, const char* desc, json props, std::vector<std::string> required)
{
    return { { "type", "function" },
             { "function", { { "name", name }, { "description", desc },
                             { "parameters", { { "type", "object" }, { "properties", props }, { "required", required } } } } } };
}
} // namespace

// Compact on purpose: the schemas are part of every prompt (cached prefix).
// Ranges live in the descriptions; "track" = index 1..maxTracks() or exact name.
bool paperDynamics()
{
    static const bool on = [] { const char* e = std::getenv ("IM_PAPER_DYNAMICS"); return e != nullptr && std::string (e) == "1"; }();
    return on;
}

const json& toolSchemas()
{
    static const json tools = [] { json t = json::array ({
        fn ("set_fader", "Track fader, db -90(-inf)..12.", { { "track", str() }, { "db", num() } }, { "track", "db" }),
        fn ("set_balance", "Track balance, value -1 left..1 right.", { { "track", str() }, { "value", num() } }, { "track", "value" }),
        fn ("set_width", "Track stereo width, value 0 mono..1 unchanged..2.", { { "track", str() }, { "value", num() } }, { "track", "value" }),
        fn ("set_doubler", "Track doubler: simulated double-tracking, two drifting copies spread left and right at constant level; low end stays "
                           "centred. Widens a mono part (sw) without panning it. mix 0-1: 0.3 thicker, 0.5 a clear double, 0.8-1 like two takes panned "
                           "hard left/right. delay_ms 5-40 (base delay), spread_ms 0-20 (L/R difference), depth_ms 0-8 (how far the timing drifts), "
                           "rate_hz 0.05-4 (how fast it drifts).",
            { { "track", str() }, { "enabled", boolean() }, { "delay_ms", num() }, { "spread_ms", num() },
              { "depth_ms", num() }, { "rate_hz", num() }, { "mix", num() } }, { "track", "enabled" }),
        fn ("set_trim", "Track input trim, db -24..24.", { { "track", str() }, { "db", num() } }, { "track", "db" }),
        fn ("set_filter", "Track filter. hpf freq 20-1000 slope 12|18|24; lpf freq 1000-20000 slope 12|24.",
            { { "track", str() }, { "type", enm ({ "hpf", "lpf" }) }, { "enabled", boolean() }, { "freq", num() }, { "slope", num() } },
            { "track", "type", "enabled", "freq", "slope" }),
        fn ("set_eq_band", "Track EQ band. freq lf 20-500, lmf 100-2000, hmf 500-8000, hf 1500-20000; gain_db -18..18; q 0.3-8; shape lf/hf only.",
            { { "track", str() }, { "band", enm ({ "lf", "lmf", "hmf", "hf" }) }, { "enabled", boolean() }, { "freq", num() },
              { "gain_db", num() }, { "q", num() }, { "shape", enm ({ "shelf", "bell" }) } },
            { "track", "band", "enabled", "freq", "gain_db" }),
        fn ("set_compressor", "Track compressor. threshold_db -60..0, ratio 1-20, attack_ms 0.1-100, release_ms 10-2000, knee_db 0-18, makeup_db 0-24.",
            { { "track", str() }, { "enabled", boolean() }, { "threshold_db", num() }, { "ratio", num() }, { "attack_ms", num() },
              { "release_ms", num() }, { "knee_db", num() }, { "makeup_db", num() } },
            { "track", "enabled" }),
        fn ("set_gate", "Track gate. threshold_db -80..0, range_db 0-80, attack_ms 0.1-50, hold_ms 0-500, release_ms 5-2000.",
            { { "track", str() }, { "enabled", boolean() }, { "threshold_db", num() }, { "range_db", num() }, { "attack_ms", num() },
              { "hold_ms", num() }, { "release_ms", num() } },
            { "track", "enabled" }),
        fn ("set_reverb_send", "Track reverb send, db -90(-inf)..6.", { { "track", str() }, { "db", num() }, { "pre_fader", boolean() } }, { "track", "db" }),
        fn ("set_echo_send", "Track send to the shared stereo echo (post-fader), db -90(-inf)..6.", { { "track", str() }, { "db", num() } }, { "track", "db" }),
        fn ("set_echo", "Shared stereo echo (delay). time_l_ms / time_r_ms 20-1500 (different L/R times = width; e.g. 1/8 and 3/16 note), "
                        "feedback 0-0.9 (repeats), low_cut_hz 20-2000, high_cut_hz 1000-20000 (each repeat gets thinner/darker), return_db -90..6.",
            { { "time_l_ms", num() }, { "time_r_ms", num() }, { "feedback", num() }, { "low_cut_hz", num() }, { "high_cut_hz", num() },
              { "return_db", num() } },
            {}),
        fn ("set_drive", "Track peak clipper / saturation: rounds off the top db of the track's peaks (the knee follows the track's own "
                         "recent peak level, so it works the same at any level) and is unity below that, so it lowers the track's cf by about db "
                         "without changing its loudness; it also adds a little harmonic density. db 0 (off)..18: 2-4 tames transients on spiky "
                         "drums or vocals, 6+ is audible saturation.", { { "track", str() }, { "db", num() } }, { "track", "db" }),
        fn ("set_deesser", "Track de-esser: turns the band above freq down when it gets loud (sibilance, harsh cymbals). "
                           "threshold_db -60..0 (lower = more reduction), freq 2000-12000 Hz.",
            { { "track", str() }, { "enabled", boolean() }, { "threshold_db", num() }, { "freq", num() } }, { "track", "enabled" }),
        fn ("set_group_bus", "Instrument-group bus: every track of one instrument (by its name, as in GROUPS) is summed here before the mix. "
                             "A slow compressor steadies the group's level (glue; set threshold_db from the GROUPS bus rms); clip_db rounds off "
                             "the top clip_db of the group's combined peaks, e.g. several drum mics hitting at once, which per-track drive cannot "
                             "reach. group vocals|drums|bass|guitars|keys; threshold_db -60..0, ratio 1-10, attack_ms 0.1-100, release_ms 10-2000, "
                             "makeup_db 0-24, clip_db 0-18.",
            { { "group", enm ({ "vocals", "drums", "bass", "guitars", "keys" }) }, { "enabled", boolean() }, { "threshold_db", num() },
              { "ratio", num() }, { "attack_ms", num() }, { "release_ms", num() }, { "makeup_db", num() }, { "clip_db", num() } },
            { "group" }),
        fn ("set_reverb", "Shared reverb. predelay_ms 0-200, decay_s 0.2-10, size 0-1, hf_damping_hz 1000-20000, low_cut_hz 20-1000, high_cut_hz 1000-20000, width 0-1, return_db -90..6.",
            { { "predelay_ms", num() }, { "decay_s", num() }, { "size", num() }, { "hf_damping_hz", num() }, { "low_cut_hz", num() },
              { "high_cut_hz", num() }, { "width", num() }, { "return_db", num() } },
            {}),
        fn ("set_bus_compressor", "Master glue compressor. threshold_db -60..0, ratio 1-10, attack_ms 0.1-100, release_ms 10-2000, makeup_db 0-24.",
            { { "enabled", boolean() }, { "threshold_db", num() }, { "ratio", num() }, { "attack_ms", num() }, { "release_ms", num() }, { "makeup_db", num() } },
            { "enabled" }),
        fn ("set_mute", "Mute a track. Only if the user asked.", { { "track", str() }, { "mute", boolean() } }, { "track", "mute" }),
        fn ("no_change", "Make no change this turn.", { { "reason", str() } }, { "reason" }),
    });
    if (paperDynamics())
    {
        json k = json::array();
        for (auto& f : t)
        {
            const auto n = f["function"]["name"].get<std::string>();
            if (n == "set_group_bus") continue;
            if (n == "set_drive")
                f["function"]["description"] = "Track saturation (soft clip): adds harmonics, density and presence. db 0 (off)..18; 2-6 subtle, 8-14 obvious.";
            k.push_back (f);
        }
        t = k;
    }
    return t; }();
    return tools;
}

// ------------------------------------------------------------------ parsing
namespace
{
std::string trim (std::string s)
{
    auto ws = [] (unsigned char c) { return std::isspace (c) != 0; };
    while (! s.empty() && ws ((unsigned char) s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && ws ((unsigned char) s[i])) ++i;
    return s.substr (i);
}

// Qwen XML-style: <function=name><parameter=key>value</parameter>...</function>
bool parseXmlStyle (const std::string& body, ToolCall& tc)
{
    static const std::regex fnRe (R"(<function=([A-Za-z_0-9]+)>)");
    static const std::regex parRe (R"(<parameter=([A-Za-z_0-9]+)>\s*([\s\S]*?)\s*</parameter>)");
    std::smatch m;
    if (! std::regex_search (body, m, fnRe)) return false;
    tc.name = m[1];
    tc.args = json::object();
    for (auto it = std::sregex_iterator (body.begin(), body.end(), parRe); it != std::sregex_iterator(); ++it)
    {
        const std::string key = (*it)[1], val = trim ((*it)[2]);
        auto parsed = json::parse (val, nullptr, false);
        tc.args[key] = parsed.is_discarded() ? json (val) : parsed;
    }
    tc.rawArgs = tc.args.dump();
    return true;
}
} // namespace

ParsedResponse parseChatResponse (const json& r)
{
    ParsedResponse out;
    if (r.contains ("usage"))
    {
        out.promptTokens = r["usage"].value ("prompt_tokens", 0);
        out.completionTokens = r["usage"].value ("completion_tokens", 0);
    }
    if (r.contains ("timings"))
    {
        out.promptMs = r["timings"].value ("prompt_ms", 0.0);
        out.predictedMs = r["timings"].value ("predicted_ms", 0.0);
    }
    if (! r.contains ("choices") || ! r["choices"].is_array() || r["choices"].empty())
    {
        out.parseError = "response has no choices";
        return out;
    }
    const auto& choice = r["choices"][0];
    out.finishReason = choice.value ("finish_reason", "");
    const auto& msg = choice.value ("message", json::object());
    std::string content = msg.contains ("content") && msg["content"].is_string() ? msg["content"].get<std::string>() : "";

    // strip <think>...</think>
    content = std::regex_replace (content, std::regex (R"(<think>[\s\S]*?</think>)"), "");

    if (msg.contains ("tool_calls") && msg["tool_calls"].is_array())
    {
        for (auto& c : msg["tool_calls"])
        {
            ToolCall tc;
            tc.id = c.value ("id", "");
            const auto& f = c.value ("function", json::object());
            tc.name = f.value ("name", "");
            if (f.contains ("arguments"))
            {
                if (f["arguments"].is_string())
                {
                    tc.rawArgs = f["arguments"].get<std::string>();
                    tc.args = json::parse (tc.rawArgs.empty() ? "{}" : tc.rawArgs, nullptr, false);
                    if (tc.args.is_discarded() || ! tc.args.is_object())
                    {
                        out.parseError = "invalid JSON arguments for " + tc.name + ": " + tc.rawArgs;
                        tc.args = json::object();
                    }
                }
                else if (f["arguments"].is_object())
                {
                    tc.args = f["arguments"];
                    tc.rawArgs = tc.args.dump();
                }
            }
            out.calls.push_back (tc);
        }
    }

    // Fallback: tool calls embedded in content as <tool_call>...</tool_call>
    static const std::regex tcRe (R"(<tool_call>([\s\S]*?)</tool_call>)");
    if (out.calls.empty())
    {
        for (auto it = std::sregex_iterator (content.begin(), content.end(), tcRe); it != std::sregex_iterator(); ++it)
        {
            const std::string body = trim ((*it)[1]);
            ToolCall tc;
            auto j = json::parse (body, nullptr, false);
            if (! j.is_discarded() && j.is_object() && j.contains ("name"))
            {
                tc.name = j.value ("name", "");
                tc.args = j.value ("arguments", json::object());
                if (tc.args.is_string()) tc.args = json::parse (tc.args.get<std::string>(), nullptr, false);
                if (tc.args.is_discarded() || ! tc.args.is_object())
                {
                    out.parseError = "invalid JSON arguments in <tool_call>: " + body;
                    tc.args = json::object();
                }
                tc.rawArgs = tc.args.dump();
                out.calls.push_back (tc);
            }
            else if (parseXmlStyle (body, tc))
                out.calls.push_back (tc);
            else
                out.parseError = "malformed <tool_call> block: " + body.substr (0, 200);
        }
    }
    content = std::regex_replace (content, tcRe, "");
    content = std::regex_replace (content, std::regex (R"(<tool_call>[\s\S]*$)"), ""); // unterminated
    out.content = trim (content);
    if (out.calls.empty() && out.parseError.empty() && out.finishReason == "length")
        out.parseError = "response truncated (max_tokens) before tool calls";
    return out;
}

// --------------------------------------------------------------- validation
int resolveTrack (const json& v, const std::vector<std::string>& names)
{
    if (v.is_number_integer() || v.is_number_float())
    {
        const double d = v.get<double>();
        const int k = (int) std::lround (d);
        return (k >= 1 && k <= maxTracks() && std::fabs (d - k) < 1e-6) ? k - 1 : -1;
    }
    if (! v.is_string()) return -1;
    auto s = trim (v.get<std::string>());
    // 1. exact name
    for (int t = 0; t < maxTracks(); ++t)
        if (names[(size_t) t] == s) return t;
    auto lower = [] (std::string x) { for (auto& c : x) c = (char) std::tolower ((unsigned char) c); return x; };
    // 2. "N", "N name" (as written in the prompt tables), "Track N"
    std::string rest = s;
    if (lower (rest).rfind ("track ", 0) == 0 && rest.size() > 6 && std::isdigit ((unsigned char) rest[6])) rest = rest.substr (6);
    size_t i = 0;
    while (i < rest.size() && std::isdigit ((unsigned char) rest[i])) ++i;
    if (i > 0 && i <= 2)
    {
        const int k = std::stoi (rest.substr (0, i));
        const auto tail = trim (rest.substr (i));
        if (k >= 1 && k <= maxTracks() && (tail.empty() || lower (tail) == lower (names[(size_t) k - 1]))) return k - 1;
    }
    // 3. unique case-insensitive name
    int found = -1;
    for (int t = 0; t < maxTracks(); ++t)
        if (lower (names[(size_t) t]) == lower (s)) { if (found >= 0) return -1; found = t; }
    return found;
}

namespace
{
std::optional<float> asNumber (const json& v)
{
    if (v.is_number()) return v.get<float>();
    if (v.is_string())
    {
        auto s = v.get<std::string>();
        if (s == "-inf" || s == "-Infinity") return kMinusInfDb;
        char* end = nullptr;
        const float f = std::strtof (s.c_str(), &end);
        if (end != s.c_str()) return f;
    }
    return std::nullopt;
}

std::optional<float> asBool (const json& v)
{
    if (v.is_boolean()) return v.get<bool>() ? 1.0f : 0.0f;
    if (v.is_number()) return v.get<double>() != 0 ? 1.0f : 0.0f;
    if (v.is_string())
    {
        auto s = v.get<std::string>();
        if (s == "true" || s == "on" || s == "1") return 1.0f;
        if (s == "false" || s == "off" || s == "0") return 0.0f;
    }
    return std::nullopt;
}

struct Validator
{
    const ToolContext& ctx;
    ValidationResult& res;
    std::string tool;

    void reject (int param, float requested, const std::string& note)
    {
        AppliedChange c;
        c.param = param >= 0 ? spec (param).id : "?";
        c.from = c.to = param >= 0 ? ctx.currentValue (param) : 0;
        c.requested = requested;
        c.status = "rejected";
        c.note = note;
        res.changes.push_back (c);
        DeltaLog d { tool, c.param, "rejected", note, c.from, requested, c.from, 0, 0 };
        if (param >= 0) d.requestedDelta = stepDelta (spec (param), c.from, requested);
        res.deltas.push_back (d);
    }

    // value: requested display value (choices as index, bools 0/1)
    void target (int param, float requested, std::string note = {})
    {
        const auto& s = spec (param);
        const float cur = ctx.currentValue (param);
        if (ctx.isLocked && ctx.isLocked (param))
        {
            reject (param, requested, "locked");
            return;
        }
        std::string status = "applied";
        float v = clampToSpec (s, requested);
        if (std::fabs (v - requested) > 1e-4f) status = "clamped";

        // Max change per tick (optional, settings table)
        const auto key = maxStepKeyFor (s);
        if (! key.empty())
        {
            auto it = ctx.settings.maxStep.find (key);
            if (it != ctx.settings.maxStep.end() && it->second)
            {
                const double lim = std::fabs (*it->second);
                const double d = stepDelta (s, cur, v);
                if (std::fabs (d) > lim)
                {
                    const double sign = d > 0 ? 1.0 : -1.0;
                    v = s.domain == Domain::LogHz ? (float) (cur * std::exp2 (sign * lim)) : (float) (cur + sign * lim);
                    v = clampToSpec (s, v);
                    status = "limited";
                    note += (note.empty() ? "" : "; ") + ("max step " + key + "=" + std::to_string (lim).substr (0, 5));
                }
            }
        }
        if (std::fabs (v - cur) < std::max (1e-6f, s.interval * 0.5f) && status == "applied") status = "unchanged"; // below display resolution

        AppliedChange c { s.id, cur, v, requested, status, note };
        // last call for a param wins: drop earlier entries
        for (auto& e : res.changes)
            if (e.param == s.id && e.status != "rejected") e.status = "superseded";
        res.changes.push_back (c);
        res.deltas.push_back ({ tool, s.id, status, note, cur, requested, v, stepDelta (s, cur, requested), stepDelta (s, cur, v) });
        res.targets.erase (std::remove_if (res.targets.begin(), res.targets.end(), [&] (auto& t) { return t.param == param; }), res.targets.end());
        if (status != "unchanged") res.targets.push_back ({ param, v });
    }

    void number (const json& args, const char* key, int param)
    {
        if (! args.contains (key) || args[key].is_null()) return; // omitted = unchanged
        auto v = asNumber (args[key]);
        if (! v) { reject (param, 0, std::string ("invalid number for ") + key); return; }
        target (param, *v);
    }

    void flag (const json& args, const char* key, int param)
    {
        if (! args.contains (key) || args[key].is_null()) return;
        auto v = asBool (args[key]);
        if (! v) { reject (param, 0, std::string ("invalid boolean for ") + key); return; }
        target (param, *v);
    }
};
} // namespace

ValidationResult validateToolCalls (const std::vector<ToolCall>& calls, const ToolContext& ctx)
{
    ValidationResult res;
    for (auto& call : calls)
    {
        Validator V { ctx, res, call.name };
        const auto& a = call.args.is_object() ? call.args : json::object();
        const auto& n = call.name;

        if (n == "no_change")
        {
            res.noChange = true;
            res.noChangeReason = a.value ("reason", "");
            continue;
        }

        const bool needsTrack = n != "set_reverb" && n != "set_bus_compressor" && n != "set_echo" && n != "set_group_bus";
        int t = -1;
        if (needsTrack)
        {
            t = a.contains ("track") ? resolveTrack (a["track"], ctx.names) : -1;
            if (t < 0)
            {
                res.errors.push_back (n + ": unknown track " + (a.contains ("track") ? a["track"].dump() : "(missing)"));
                V.reject (-1, 0, "unknown track");
                continue;
            }
            if (! ctx.presentMask.has (t))
            {
                res.errors.push_back (n + ": track " + std::to_string (t + 1) + " is not active");
                V.reject (trackParam (t, Fader), 0, "track not active");
                continue;
            }
        }
        auto P = [&] (int slot) { return trackParam (t, slot); };
        auto M = [&] (int slot) { return masterParam (slot); };

        if (n == "set_fader") V.number (a, "db", P (Fader));
        else if (n == "set_balance") V.number (a, "value", P (Balance));
        else if (n == "set_width") V.number (a, "value", P (Width));
        else if (n == "set_doubler")
        {
            V.flag (a, "enabled", P (DblOn));
            V.number (a, "delay_ms", P (DblDelay));
            V.number (a, "spread_ms", P (DblSpread));
            V.number (a, "depth_ms", P (DblDepth));
            V.number (a, "rate_hz", P (DblRate));
            V.number (a, "mix", P (DblMix));
        }
        else if (n == "set_trim") V.number (a, "db", P (TrimGain));
        else if (n == "set_filter")
        {
            const auto type = a.value ("type", std::string());
            if (type != "hpf" && type != "lpf")
            {
                res.errors.push_back ("set_filter: type must be hpf or lpf");
                V.reject (-1, 0, "invalid filter type");
                continue;
            }
            const bool hp = type == "hpf";
            V.flag (a, "enabled", P (hp ? HpfOn : LpfOn));
            V.number (a, "freq", P (hp ? HpfFreq : LpfFreq));
            if (a.contains ("slope") && ! a["slope"].is_null())
            {
                auto sl = asNumber (a["slope"]);
                if (! sl) V.reject (P (hp ? HpfSlope : LpfSlope), 0, "invalid slope");
                else
                {
                    // map dB/oct to the nearest available choice index
                    const std::vector<int> opts = hp ? std::vector<int> { 12, 18, 24 } : std::vector<int> { 12, 24 };
                    int best = 0;
                    for (int i = 1; i < (int) opts.size(); ++i)
                        if (std::fabs (*sl - (float) opts[(size_t) i]) < std::fabs (*sl - (float) opts[(size_t) best])) best = i;
                    const bool exact = std::fabs (*sl - (float) opts[(size_t) best]) < 0.5f;
                    V.target (P (hp ? HpfSlope : LpfSlope), (float) best,
                              exact ? "" : "slope " + std::to_string ((int) *sl) + " -> " + std::to_string (opts[(size_t) best]));
                    if (! exact && res.changes.back().status != "rejected") res.changes.back().status = "clamped";
                }
            }
        }
        else if (n == "set_eq_band")
        {
            const auto band = a.value ("band", std::string());
            int on, fr, gn, q, shape = -1;
            if (band == "lf") { on = LfOn; fr = LfFreq; gn = LfGain; q = LfQ; shape = LfShape; }
            else if (band == "lmf") { on = LmfOn; fr = LmfFreq; gn = LmfGain; q = LmfQ; }
            else if (band == "hmf") { on = HmfOn; fr = HmfFreq; gn = HmfGain; q = HmfQ; }
            else if (band == "hf") { on = HfOn; fr = HfFreq; gn = HfGain; q = HfQ; shape = HfShape; }
            else
            {
                res.errors.push_back ("set_eq_band: band must be lf|lmf|hmf|hf");
                V.reject (-1, 0, "invalid band");
                continue;
            }
            V.flag (a, "enabled", P (on));
            if (asBool (a.value ("enabled", json())).value_or (0.0f) > 0.5f && ctx.currentValue (P (EqOn)) < 0.5f)
                V.target (P (EqOn), 1.0f, "EQ module enabled for band");
            V.number (a, "freq", P (fr));
            V.number (a, "gain_db", P (gn));
            V.number (a, "q", P (q));
            if (a.contains ("shape") && ! a["shape"].is_null())
            {
                const auto sh = a["shape"].is_string() ? a["shape"].get<std::string>() : std::string();
                if (shape < 0) { if (sh != "bell") V.reject (P (on), 0, "shape only for lf/hf"); }
                else if (sh == "shelf" || sh == "bell") V.target (P (shape), sh == "shelf" ? 0.0f : 1.0f);
                else V.reject (P (shape), 0, "invalid shape");
            }
        }
        else if (n == "set_compressor")
        {
            V.flag (a, "enabled", P (CompOn));
            V.number (a, "threshold_db", P (CompThr));
            V.number (a, "ratio", P (CompRatio));
            V.number (a, "attack_ms", P (CompAtt));
            V.number (a, "release_ms", P (CompRel));
            V.number (a, "knee_db", P (CompKnee));
            V.number (a, "makeup_db", P (CompMakeup));
        }
        else if (n == "set_gate")
        {
            V.flag (a, "enabled", P (GateOn));
            V.number (a, "threshold_db", P (GateThr));
            V.number (a, "range_db", P (GateRange));
            V.number (a, "attack_ms", P (GateAtt));
            V.number (a, "hold_ms", P (GateHold));
            V.number (a, "release_ms", P (GateRel));
        }
        else if (n == "set_reverb_send")
        {
            V.number (a, "db", P (SendGain));
            V.flag (a, "pre_fader", P (SendPre));
        }
        else if (n == "set_echo_send") V.number (a, "db", P (EchoSend));
        else if (n == "set_echo")
        {
            V.number (a, "time_l_ms", M (EchoTimeL));
            V.number (a, "time_r_ms", M (EchoTimeR));
            V.number (a, "feedback", M (EchoFeedback));
            V.number (a, "low_cut_hz", M (EchoLowCut));
            V.number (a, "high_cut_hz", M (EchoHighCut));
            V.number (a, "return_db", M (EchoReturn));
        }
        else if (n == "set_drive") V.number (a, "db", P (Drive));
        else if (n == "set_deesser")
        {
            V.flag (a, "enabled", P (DsOn));
            V.number (a, "threshold_db", P (DsThr));
            V.number (a, "freq", P (DsFreq));
        }
        else if (n == "set_reverb")
        {
            V.number (a, "predelay_ms", M (RevPredelay));
            V.number (a, "decay_s", M (RevDecay));
            V.number (a, "size", M (RevSize));
            V.number (a, "hf_damping_hz", M (RevHfDamp));
            V.number (a, "low_cut_hz", M (RevLowCut));
            V.number (a, "high_cut_hz", M (RevHighCut));
            V.number (a, "width", M (RevWidth));
            V.number (a, "return_db", M (RevReturn));
        }
        else if (n == "set_group_bus")
        {
            static const char* names[kNumGroupBuses] = { "vocals", "drums", "bass", "guitars", "keys" };
            const auto gname = a.value ("group", std::string());
            int g = -1;
            for (int k = 0; k < kNumGroupBuses; ++k)
                if (gname == names[k]) g = k;
            if (g < 0)
            {
                res.errors.push_back ("set_group_bus: group must be vocals|drums|bass|guitars|keys");
                V.reject (-1, 0, "invalid group");
                continue;
            }
            auto B = [&] (int f) { return masterParam (groupBusSlot (g, f)); };
            V.flag (a, "enabled", B (GbOn));
            V.number (a, "threshold_db", B (GbThr));
            V.number (a, "ratio", B (GbRatio));
            V.number (a, "attack_ms", B (GbAtt));
            V.number (a, "release_ms", B (GbRel));
            V.number (a, "makeup_db", B (GbMakeup));
            V.number (a, "clip_db", B (GbClip));
        }
        else if (n == "set_bus_compressor")
        {
            V.flag (a, "enabled", M (BcOn));
            V.number (a, "threshold_db", M (BcThr));
            V.number (a, "ratio", M (BcRatio));
            V.number (a, "attack_ms", M (BcAtt));
            V.number (a, "release_ms", M (BcRel));
            V.number (a, "makeup_db", M (BcMakeup));
        }
        else if (n == "set_mute")
        {
            if (! ctx.userRequest)
            {
                V.reject (P (Mute), asBool (a.value ("mute", json())).value_or (0.0f), "mute only when the user asked");
                continue;
            }
            V.flag (a, "mute", P (Mute));
        }
        else
        {
            res.errors.push_back ("unknown tool: " + n);
            V.reject (-1, 0, "unknown tool " + n);
        }
    }
    return res;
}
} // namespace im
