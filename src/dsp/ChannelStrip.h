#pragma once
// Per-track stereo-linked console channel strip.
// Order: Trim -> Polarity -> HPF -> LPF -> Gate -> [EQ <-> Comp] -> De-esser -> Drive -> Width -> Balance -> Fader -> Mute/Solo
// Reverb send tapped pre-fader (after Balance) or post-fader; mute applies to both.
//
// Discrete parameters (on/off, slopes, shapes, order, polarity, send pre/post)
// switch with an equal-power crossfade between the old and the new processing
// path. Up to kCores paths run concurrently: on each change a free core is
// cloned from the newest one (warm filter/detector state), reconfigured, and
// faded in while every other active core fades out (powers sum to 1).
#include "dsp/Doubler.h"
#include "dsp/Dynamics.h"
#include "dsp/Filters.h"
#include <array>
#include <vector>

namespace im
{
struct StripConfig
{
    bool polL = false, polR = false;
    bool hpfOn = false;
    int hpfSlope = 12;
    bool lpfOn = false;
    int lpfSlope = 12;
    bool gateOn = false;
    bool eqOn = true;
    std::array<bool, 4> bandOn { true, true, true, true };
    bool lfShelf = true, hfShelf = true;
    bool compOn = false, autoMakeup = false;
    bool compFirst = false;
    bool dsOn = false;
    bool sendPre = false;

    bool operator== (const StripConfig&) const = default;
};

struct StripBlockParams
{
    float hpfFreq = 80, lpfFreq = 18000;
    GateParams gate;
    std::array<float, 4> eqFreq { 100, 400, 2500, 8000 }, eqGain {}, eqQ { 0.7f, 1, 1, 0.7f };
    CompressorParams comp;
    float dsThrDb = -30, dsFreq = 6500;
    float driveDb = 0;
};

// Per-sample gain curves for this block (computed once per block, shared by cores).
struct StripGains
{
    const float* trim;
    const float* width;
    const float* balL;
    const float* balR;
    const float* fader;
    const float* send;
    const float* mute;
};

struct StripCore
{
    StripConfig cfg;
    ButterworthCascade hpf, lpf;
    std::array<StereoBiquad, 4> eq;
    Gate gate;
    Compressor comp;
    float compGrMax = 0, gateGrMax = 0;
    // de-esser: the band above dsFreq (input minus its low-pass, so with no reduction the input passes exactly) is
    // turned down 4:1 above the threshold, at most 12 dB, when it gets loud
    StereoBiquad dsLp;
    float dsEnv = 0, dsThrDb = -30, dsAtt = 0, dsRel = 0, dsGrMax = 0;
    PeakClipper drive; // rounds off the top driveDb of the track's peaks (Dynamics.h)

    void prepare (double fs);
    void reset();
    void updateCoeffs (const StripBlockParams& p, double fs);
    void process (const float* inL, const float* inR, int n, const StripGains& g,
                  float* outL, float* outR, float* sendL, float* sendR) noexcept;
};

class ChannelStrip
{
public:
    static constexpr int kCores = 3;

    void prepare (double sampleRate, int maxBlock);
    void reset();

    // Request a new discrete configuration, crossfaded over rampSamples.
    void setConfig (const StripConfig& c, int rampSamples) noexcept;
    // Doubler runs last, on the summed core output (not the reverb send).
    void setDoubler (const DoublerParams& p) noexcept { doubler.setParams (p); }
    const StripConfig& targetConfig() const noexcept { return hasPending ? pendingCfg : cores[(size_t) newest].cfg; }

    void process (const float* inL, const float* inR, int n, const StripBlockParams& p, const StripGains& g,
                  float* outL, float* outR, float* sendL, float* sendR) noexcept;

    float compGainReductionDb() const noexcept { return cores[(size_t) newest].compGrMax; }
    float gateGainReductionDb() const noexcept { return cores[(size_t) newest].gateGrMax; }
    bool isCrossfading() const noexcept { return xfading; }
    int activeCoreCount() const noexcept;

private:
    void startTransition (const StripConfig& c, int rampSamples) noexcept;
    float corePower (int k, float t) const noexcept { return k == newest ? t : power0[(size_t) k] * (1.0f - t); }

    double fs = 48000.0;
    Doubler doubler;
    std::array<StripCore, kCores> cores;
    std::array<bool, kCores> active { true, false, false };
    std::array<float, kCores> power0 {};
    int newest = 0;
    bool xfading = false;
    int xfadePos = 0, xfadeLen = 1;
    bool hasPending = false;
    StripConfig pendingCfg;
    int pendingRamp = 0;
    std::array<std::array<std::vector<float>, 4>, kCores> scratch; // [core][outL,outR,sendL,sendR]
};
} // namespace im
