#include "core/Params.h"
#include <atomic>
#include <cmath>
#include <unordered_map>

namespace im
{
namespace
{
struct Def
{
    int slot;
    const char* module;
    const char* name;
    Kind kind;
    Domain domain;
    float min, max, def, interval;
    int decimals;
    const char* unit;
    std::vector<std::string> choices = {};
    bool minusInf = false;
};

const std::vector<Def>& trackDefs()
{
    static const std::vector<Def> d = {
        { TrimGain, "trim", "gain", Kind::Float, Domain::Decibel, -24, 24, 0, 0.1f, 1, "dB" },
        { PolL, "polarity", "inv_l", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
        { PolR, "polarity", "inv_r", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
        { HpfOn, "hpf", "on", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
        { HpfFreq, "hpf", "freq", Kind::Float, Domain::LogHz, 20, 1000, 80, 1, 0, "Hz" },
        { HpfSlope, "hpf", "slope", Kind::Choice, Domain::Discrete, 0, 2, 0, 1, 0, "dB/oct", { "12", "18", "24" } },
        { LpfOn, "lpf", "on", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
        { LpfFreq, "lpf", "freq", Kind::Float, Domain::LogHz, 1000, 20000, 18000, 1, 0, "Hz" },
        { LpfSlope, "lpf", "slope", Kind::Choice, Domain::Discrete, 0, 1, 0, 1, 0, "dB/oct", { "12", "24" } },
        { GateOn, "gate", "on", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
        { GateThr, "gate", "threshold", Kind::Float, Domain::Decibel, -80, 0, -50, 0.1f, 1, "dB" },
        { GateRange, "gate", "range", Kind::Float, Domain::Decibel, 0, 80, 40, 0.1f, 1, "dB" },
        { GateAtt, "gate", "attack", Kind::Float, Domain::Linear, 0.1f, 50, 1, 0.01f, 1, "ms" },
        { GateHold, "gate", "hold", Kind::Float, Domain::Linear, 0, 500, 50, 0.1f, 0, "ms" },
        { GateRel, "gate", "release", Kind::Float, Domain::Linear, 5, 2000, 100, 0.1f, 0, "ms" },
        { EqOn, "eq", "on", Kind::Bool, Domain::Discrete, 0, 1, 1, 1, 0, "" },
        { LfOn, "eq", "lf_on", Kind::Bool, Domain::Discrete, 0, 1, 1, 1, 0, "" },
        { LfFreq, "eq", "lf_freq", Kind::Float, Domain::LogHz, 20, 500, 100, 1, 0, "Hz" },
        { LfGain, "eq", "lf_gain", Kind::Float, Domain::Decibel, -18, 18, 0, 0.1f, 1, "dB" },
        { LfQ, "eq", "lf_q", Kind::Float, Domain::Linear, 0.3f, 8, 0.7f, 0.01f, 2, "" },
        { LfShape, "eq", "lf_shape", Kind::Choice, Domain::Discrete, 0, 1, 0, 1, 0, "", { "shelf", "bell" } },
        { LmfOn, "eq", "lmf_on", Kind::Bool, Domain::Discrete, 0, 1, 1, 1, 0, "" },
        { LmfFreq, "eq", "lmf_freq", Kind::Float, Domain::LogHz, 100, 2000, 400, 1, 0, "Hz" },
        { LmfGain, "eq", "lmf_gain", Kind::Float, Domain::Decibel, -18, 18, 0, 0.1f, 1, "dB" },
        { LmfQ, "eq", "lmf_q", Kind::Float, Domain::Linear, 0.3f, 8, 1.0f, 0.01f, 2, "" },
        { HmfOn, "eq", "hmf_on", Kind::Bool, Domain::Discrete, 0, 1, 1, 1, 0, "" },
        { HmfFreq, "eq", "hmf_freq", Kind::Float, Domain::LogHz, 500, 8000, 2500, 1, 0, "Hz" },
        { HmfGain, "eq", "hmf_gain", Kind::Float, Domain::Decibel, -18, 18, 0, 0.1f, 1, "dB" },
        { HmfQ, "eq", "hmf_q", Kind::Float, Domain::Linear, 0.3f, 8, 1.0f, 0.01f, 2, "" },
        { HfOn, "eq", "hf_on", Kind::Bool, Domain::Discrete, 0, 1, 1, 1, 0, "" },
        { HfFreq, "eq", "hf_freq", Kind::Float, Domain::LogHz, 1500, 20000, 8000, 1, 0, "Hz" },
        { HfGain, "eq", "hf_gain", Kind::Float, Domain::Decibel, -18, 18, 0, 0.1f, 1, "dB" },
        { HfQ, "eq", "hf_q", Kind::Float, Domain::Linear, 0.3f, 8, 0.7f, 0.01f, 2, "" },
        { HfShape, "eq", "hf_shape", Kind::Choice, Domain::Discrete, 0, 1, 0, 1, 0, "", { "shelf", "bell" } },
        { CompOn, "comp", "on", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
        { CompThr, "comp", "threshold", Kind::Float, Domain::Decibel, -60, 0, -20, 0.1f, 1, "dB" },
        { CompRatio, "comp", "ratio", Kind::Float, Domain::Linear, 1, 20, 4, 0.1f, 1, ":1" },
        { CompAtt, "comp", "attack", Kind::Float, Domain::Linear, 0.1f, 100, 10, 0.01f, 1, "ms" },
        { CompRel, "comp", "release", Kind::Float, Domain::Linear, 10, 2000, 100, 0.1f, 0, "ms" },
        { CompKnee, "comp", "knee", Kind::Float, Domain::Decibel, 0, 18, 6, 0.1f, 1, "dB" },
        { CompMakeup, "comp", "makeup", Kind::Float, Domain::Decibel, 0, 24, 0, 0.1f, 1, "dB" },
        { CompAutoMakeup, "comp", "auto_makeup", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
        { Order, "strip", "order", Kind::Choice, Domain::Discrete, 0, 1, 0, 1, 0, "", { "eq_comp", "comp_eq" } },
        { DsOn, "deesser", "on", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
        { DsThr, "deesser", "threshold", Kind::Float, Domain::Decibel, -60, 0, -30, 0.1f, 1, "dB" },
        { DsFreq, "deesser", "freq", Kind::Float, Domain::LogHz, 2000, 12000, 6500, 1, 0, "Hz" },
        { Drive, "drive", "amount", Kind::Float, Domain::Linear, 0, 18, 0, 0.1f, 1, "dB" },
        { DblOn, "doubler", "on", Kind::Bool, Domain::Linear, 0, 1, 0, 1, 0, "" },
        { DblDelay, "doubler", "delay", Kind::Float, Domain::Linear, 5, 40, 18, 0.5f, 1, "ms" },
        { DblSpread, "doubler", "spread", Kind::Float, Domain::Linear, 0, 20, 8, 0.5f, 1, "ms" },
        { DblDepth, "doubler", "depth", Kind::Float, Domain::Linear, 0, 8, 2, 0.1f, 1, "ms" },
        { DblRate, "doubler", "rate", Kind::Float, Domain::Linear, 0.05f, 4, 0.7f, 0.05f, 2, "Hz" },
        { DblMix, "doubler", "mix", Kind::Float, Domain::Linear, 0, 1, 0.5f, 0.01f, 2, "" },
        { Width, "width", "amount", Kind::Float, Domain::Linear, 0, 2, 1, 0.01f, 2, "" },
        { Balance, "balance", "value", Kind::Float, Domain::Linear, -1, 1, 0, 0.01f, 2, "" },
        { Fader, "fader", "gain", Kind::Float, Domain::Decibel, kMinusInfDb, 12, 0, 0.1f, 1, "dB", {}, true },
        { SendGain, "send", "gain", Kind::Float, Domain::Decibel, kMinusInfDb, 6, kMinusInfDb, 0.1f, 1, "dB", {}, true },
        { SendPre, "send", "pre", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
        { EchoSend, "echo_send", "gain", Kind::Float, Domain::Decibel, kMinusInfDb, 6, kMinusInfDb, 0.1f, 1, "dB", {}, true },
        { Mute, "mute", "on", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
        { Solo, "solo", "on", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
    };
    return d;
}

const std::vector<Def>& masterDefs()
{
    static const std::vector<Def> d = [] {
    std::vector<Def> v = {
        { RevPredelay, "reverb", "predelay", Kind::Float, Domain::Linear, 0, 200, 20, 0.1f, 0, "ms" },
        { RevDecay, "reverb", "decay", Kind::Float, Domain::Linear, 0.2f, 10, 1.8f, 0.01f, 2, "s" },
        { RevSize, "reverb", "size", Kind::Float, Domain::Linear, 0, 1, 0.5f, 0.01f, 2, "" },
        { RevHfDamp, "reverb", "hf_damping", Kind::Float, Domain::LogHz, 1000, 20000, 8000, 1, 0, "Hz" },
        { RevLowCut, "reverb", "low_cut", Kind::Float, Domain::LogHz, 20, 1000, 200, 1, 0, "Hz" },
        { RevHighCut, "reverb", "high_cut", Kind::Float, Domain::LogHz, 1000, 20000, 12000, 1, 0, "Hz" },
        { RevWidth, "reverb", "width", Kind::Float, Domain::Linear, 0, 1, 1, 0.01f, 2, "" },
        { RevEr, "reverb", "er_level", Kind::Float, Domain::Decibel, kMinusInfDb, 0, -12, 0.1f, 1, "dB", {}, true },
        { RevReturn, "reverb", "return", Kind::Float, Domain::Decibel, kMinusInfDb, 6, 0, 0.1f, 1, "dB", {}, true },
        { EchoTimeL, "echo", "time_l", Kind::Float, Domain::Linear, 20, 1500, 250, 1, 0, "ms" },
        { EchoTimeR, "echo", "time_r", Kind::Float, Domain::Linear, 20, 1500, 375, 1, 0, "ms" },
        { EchoFeedback, "echo", "feedback", Kind::Float, Domain::Linear, 0, 0.9f, 0.25f, 0.01f, 2, "" },
        { EchoLowCut, "echo", "low_cut", Kind::Float, Domain::LogHz, 20, 2000, 250, 1, 0, "Hz" },
        { EchoHighCut, "echo", "high_cut", Kind::Float, Domain::LogHz, 1000, 20000, 6000, 1, 0, "Hz" },
        { EchoReturn, "echo", "return", Kind::Float, Domain::Decibel, kMinusInfDb, 6, 0, 0.1f, 1, "dB", {}, true },
        { BcOn, "buscomp", "on", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
        { BcThr, "buscomp", "threshold", Kind::Float, Domain::Decibel, -60, 0, -10, 0.1f, 1, "dB" },
        { BcRatio, "buscomp", "ratio", Kind::Float, Domain::Linear, 1, 10, 2, 0.1f, 1, ":1" },
        { BcAtt, "buscomp", "attack", Kind::Float, Domain::Linear, 0.1f, 100, 10, 0.01f, 1, "ms" },
        { BcRel, "buscomp", "release", Kind::Float, Domain::Linear, 10, 2000, 200, 0.1f, 0, "ms" },
        { BcKnee, "buscomp", "knee", Kind::Float, Domain::Decibel, 0, 18, 6, 0.1f, 1, "dB" },
        { BcMakeup, "buscomp", "makeup", Kind::Float, Domain::Decibel, 0, 24, 0, 0.1f, 1, "dB" },
        { BcAutoMakeup, "buscomp", "auto_makeup", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" },
        { MasterFader, "fader", "gain", Kind::Float, Domain::Decibel, kMinusInfDb, 6, 0, 0.1f, 1, "dB", {}, true },
        { LimOn, "limiter", "on", Kind::Bool, Domain::Discrete, 0, 1, 1, 1, 0, "" },
        { LimCeiling, "limiter", "ceiling", Kind::Float, Domain::Decibel, -1, 0, -1, 0.01f, 2, "dBFS" },
    };
    // instrument-group buses
    static const char* mods[kNumGroupBuses] = { "bus_vocals", "bus_drums", "bus_bass", "bus_guitars", "bus_keys" };
    for (int g = 0; g < kNumGroupBuses; ++g)
    {
        const char* m = mods[g];
        v.push_back ({ groupBusSlot (g, GbOn), m, "on", Kind::Bool, Domain::Discrete, 0, 1, 0, 1, 0, "" });
        v.push_back ({ groupBusSlot (g, GbThr), m, "threshold", Kind::Float, Domain::Decibel, -60, 0, -20, 0.1f, 1, "dB" });
        v.push_back ({ groupBusSlot (g, GbRatio), m, "ratio", Kind::Float, Domain::Linear, 1, 10, 3, 0.1f, 1, ":1" });
        v.push_back ({ groupBusSlot (g, GbAtt), m, "attack", Kind::Float, Domain::Linear, 0.1f, 100, 10, 0.01f, 1, "ms" });
        v.push_back ({ groupBusSlot (g, GbRel), m, "release", Kind::Float, Domain::Linear, 10, 2000, 150, 0.1f, 0, "ms" });
        v.push_back ({ groupBusSlot (g, GbMakeup), m, "makeup", Kind::Float, Domain::Decibel, 0, 24, 0, 0.1f, 1, "dB" });
        v.push_back ({ groupBusSlot (g, GbClip), m, "clip", Kind::Float, Domain::Linear, 0, 18, 0, 0.1f, 1, "dB" });
    }
    return v;
    }();
    return d;
}

ParamSpec makeSpec (const Def& d, int track0)
{
    ParamSpec s;
    s.track = track0;
    s.slot = d.slot;
    s.module = d.module;
    s.name = d.name;
    s.key = s.module + "." + s.name;
    s.id = (track0 >= 0 ? "t" + std::to_string (track0 + 1) : std::string ("master")) + "." + s.key;
    s.index = track0 >= 0 ? trackParam (track0, d.slot) : masterParam (d.slot);
    s.kind = d.kind;
    s.domain = d.domain;
    s.min = d.min;
    s.max = d.max;
    s.def = d.def;
    s.interval = d.interval;
    s.decimals = (float) d.decimals;
    s.unit = d.unit;
    s.choices = d.choices;
    s.minusInf = d.minusInf;
    return s;
}

struct Registry
{
    std::vector<ParamSpec> specs;
    std::unordered_map<std::string, int> byId;

    Registry()
    {
        specs.resize ((size_t) numParams());
        for (int t = 0; t < maxTracks(); ++t)
            for (auto& d : trackDefs())
            {
                auto s = makeSpec (d, t);
                specs[(size_t) s.index] = s;
            }
        for (auto& d : masterDefs())
        {
            auto s = makeSpec (d, -1);
            specs[(size_t) s.index] = s;
        }
        for (auto& s : specs)
        {
            jassert (! s.id.empty());
            byId[s.id] = s.index;
        }
    }
};

std::atomic<int> gMaxTracks { 32 };
std::atomic<bool> gRegistryBuilt { false };

const Registry& reg()
{
    static const Registry r = [] { gRegistryBuilt = true; return Registry(); }();
    return r;
}
} // namespace

void setMaxTracks (int n)
{
    if (! gRegistryBuilt.load()) gMaxTracks = std::max (1, n);
}

int maxTracks() noexcept { return gMaxTracks.load (std::memory_order_relaxed); }

const std::vector<ParamSpec>& allParams() { return reg().specs; }
const ParamSpec& spec (int index) { return reg().specs[(size_t) index]; }

int findParam (const std::string& id)
{
    auto& m = reg().byId;
    auto it = m.find (id);
    return it == m.end() ? -1 : it->second;
}

std::string trackParamId (int track0, int slot) { return spec (trackParam (track0, slot)).id; }
std::string masterParamId (int slot) { return spec (masterParam (slot)).id; }

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (auto& s : allParams())
    {
        juce::ParameterID pid { s.id, 1 };
        auto unit = s.unit;
        auto label = juce::String (s.id);
        auto sCopy = s;
        if (s.kind == Kind::Bool)
        {
            layout.add (std::make_unique<juce::AudioParameterBool> (pid, label, s.def > 0.5f));
        }
        else if (s.kind == Kind::Choice)
        {
            juce::StringArray ch;
            for (auto& c : s.choices) ch.add (c);
            layout.add (std::make_unique<juce::AudioParameterChoice> (pid, label, ch, (int) s.def));
        }
        else
        {
            juce::NormalisableRange<float> range;
            if (s.domain == Domain::LogHz)
            {
                const float lo = s.min, hi = s.max;
                range = juce::NormalisableRange<float> (
                    lo, hi,
                    [] (float a, float b, float n) { return a * std::pow (b / a, n); },
                    [] (float a, float b, float v) { return std::log (v / a) / std::log (b / a); },
                    [] (float a, float b, float v) { return juce::jlimit (a, b, std::round (v)); });
            }
            else
            {
                range = juce::NormalisableRange<float> (s.min, s.max, s.interval);
                if (s.minusInf && s.max > 0.0f) // faders/sends: put 0 dB at ~75% of travel
                {
                    const float frac = (0.0f - s.min) / (s.max - s.min);
                    range.skew = std::log (0.75f) / std::log (frac);
                }
                else if (s.minusInf) // -inf..0 dB (e.g. ER level): -18 dB at mid travel
                    range.setSkewForCentre (-18.0f);
                else if (s.unit == "ms" || s.key == "comp.ratio" || s.key == "buscomp.ratio" || s.key == "reverb.decay")
                    range.setSkewForCentre (s.min + (s.max - s.min) * 0.15f);
            }
            juce::AudioParameterFloatAttributes attrs;
            attrs = attrs.withLabel (unit).withStringFromValueFunction ([sCopy] (float v, int) { return juce::String (formatValue (sCopy, v, false)); });
            layout.add (std::make_unique<juce::AudioParameterFloat> (pid, label, range, s.def, attrs));
        }
    }
    return layout;
}

float toRampDomain (const ParamSpec& s, float v)
{
    switch (s.domain)
    {
        case Domain::LogHz: return std::log2 (std::max (v, 1.0f));
        default: return v; // dB values are already in the dB domain
    }
}

float fromRampDomain (const ParamSpec& s, float d)
{
    switch (s.domain)
    {
        case Domain::LogHz: return std::exp2 (d);
        default: return d;
    }
}

float clampToSpec (const ParamSpec& s, float v)
{
    if (! std::isfinite (v)) v = (v < 0 && s.minusInf) ? s.min : s.def;
    if (s.kind != Kind::Float) return (float) juce::jlimit ((int) s.min, (int) s.max, (int) std::lround (v));
    return juce::jlimit (s.min, s.max, v);
}

std::string formatValue (const ParamSpec& s, float v, bool withUnit)
{
    if (s.kind == Kind::Bool) return v > 0.5f ? "on" : "off";
    if (s.kind == Kind::Choice)
    {
        auto i = (size_t) juce::jlimit (0, (int) s.choices.size() - 1, (int) std::lround (v));
        return s.choices[i] + (withUnit && ! s.unit.empty() ? " " + s.unit : "");
    }
    if (s.minusInf && v <= s.min + 1e-3f) return withUnit ? "-inf dB" : "-inf";
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.*f", (int) s.decimals, (double) v);
    std::string out = buf;
    if (out == "-0.0" || out == "-0.00" || out == "-0") out = out.substr (1);
    if (withUnit && ! s.unit.empty()) out += (s.unit == ":1" ? "" : " ") + s.unit;
    return out;
}
} // namespace im
