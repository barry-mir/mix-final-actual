#pragma once
// Parameter registry: single source of truth for every DSP parameter.
// Used by the APVTS layout, the realtime ParamEngine (ramp domain), the agent
// tool validator (ranges), and the prompt serializer (names, units, rounding).
#include <juce_audio_processors/juce_audio_processors.h>
#include <string>
#include <vector>

namespace im
{
// Track count: no fixed limit. An app process mixes one session, so the count is set once, from the session, before
// the parameter registry or any engine is built (setMaxTracks), and every per-track array is sized from it.
void setMaxTracks (int n); // n >= 1; has no effect once the registry exists
int maxTracks() noexcept;  // 32 until set

// The tracks present: the loaded session's tracks are always 0 .. n-1.
struct TrackMask
{
    int n = 0;
    constexpr bool has (int t) const noexcept { return t >= 0 && t < n; }
    constexpr int count() const noexcept { return n; }
    constexpr bool operator== (const TrackMask&) const = default;
};
constexpr float kMinusInfDb = -90.0f; // values at/below this are treated as -inf (gain 0)

// Per-track parameter slots. ID = "t{k}.{module}.{param}", k is 1-based.
enum TP : int
{
    TrimGain, PolL, PolR,
    HpfOn, HpfFreq, HpfSlope,
    LpfOn, LpfFreq, LpfSlope,
    GateOn, GateThr, GateRange, GateAtt, GateHold, GateRel,
    EqOn,
    LfOn, LfFreq, LfGain, LfQ, LfShape,
    LmfOn, LmfFreq, LmfGain, LmfQ,
    HmfOn, HmfFreq, HmfGain, HmfQ,
    HfOn, HfFreq, HfGain, HfQ, HfShape,
    CompOn, CompThr, CompRatio, CompAtt, CompRel, CompKnee, CompMakeup, CompAutoMakeup,
    Order,
    DsOn, DsThr, DsFreq,
    Drive,
    DblOn, DblDelay, DblSpread, DblDepth, DblRate, DblMix,
    Width, Balance, Fader,
    SendGain, SendPre,
    EchoSend,
    Mute, Solo,
    kNumTrackParams
};

// Master parameter slots. ID = "master.{module}.{param}".
enum MP : int
{
    RevPredelay, RevDecay, RevSize, RevHfDamp, RevLowCut, RevHighCut, RevWidth, RevEr, RevReturn,
    EchoTimeL, EchoTimeR, EchoFeedback, EchoLowCut, EchoHighCut, EchoReturn,
    BcOn, BcThr, BcRatio, BcAtt, BcRel, BcKnee, BcMakeup, BcAutoMakeup,
    MasterFader,
    LimOn, LimCeiling,
    GroupBusFirst, // 5 instrument-group buses (vocals, drums, bass, guitars, keys/other) x kGbStride fields
    GroupBusLast = GroupBusFirst + 5 * 7 - 1,
    kNumMasterParams
};

// Instrument-group buses: tracks are routed by the role their name implies (priors::roleForName).
constexpr int kNumGroupBuses = 5;
enum GbField : int { GbOn, GbThr, GbRatio, GbAtt, GbRel, GbMakeup, GbClip, kGbStride };
static_assert (GroupBusLast - GroupBusFirst + 1 == kNumGroupBuses * kGbStride);
inline constexpr int groupBusSlot (int g, int f) { return GroupBusFirst + g * kGbStride + f; }

inline int numParams() noexcept { return maxTracks() * kNumTrackParams + kNumMasterParams; }

inline int trackParam (int track0, int slot) noexcept { return track0 * kNumTrackParams + slot; }
inline int masterParam (int slot) noexcept { return maxTracks() * kNumTrackParams + slot; }

enum class Kind { Float, Bool, Choice };
// Ramp/interpolation domain: dB for gains, log-frequency for Hz, linear otherwise.
enum class Domain { Linear, Decibel, LogHz, Discrete };

struct ParamSpec
{
    int index = 0;
    std::string id;     // "t3.eq.lf_gain"
    std::string module; // "eq"
    std::string name;   // "lf_gain"
    std::string key;    // "eq.lf_gain" (module.name), used in prompts/tables
    int track = -1;     // 0-based, -1 = master
    int slot = 0;       // TP or MP value
    Kind kind = Kind::Float;
    Domain domain = Domain::Linear;
    float min = 0, max = 1, def = 0;
    float interval = 0.0f;   // display resolution (0 = continuous)
    float decimals = 1;      // prompt rounding: number of decimals
    std::vector<std::string> choices;
    std::string unit;        // "dB","Hz","ms","s",""
    bool minusInf = false;   // min value means -inf
};

const std::vector<ParamSpec>& allParams();
const ParamSpec& spec (int index);
int findParam (const std::string& id); // -1 if missing
std::string trackParamId (int track0, int slot);
std::string masterParamId (int slot);

juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

// Display-domain <-> ramp-domain transforms.
float toRampDomain (const ParamSpec& s, float v);
float fromRampDomain (const ParamSpec& s, float d);
inline float dbToGain (float db) { return db <= kMinusInfDb ? 0.0f : std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) { return g <= 1.0e-9f ? -180.0f : 20.0f * std::log10 (g); }

// Clamp a display value into the spec's range (choices/bools -> integer index).
float clampToSpec (const ParamSpec& s, float v);
// Human readable value, e.g. "-4.0 dB", "120 Hz", "on", "24".
std::string formatValue (const ParamSpec& s, float v, bool withUnit = true);
} // namespace im
