#pragma once
// The complete realtime mixer: kMaxTracks channel strips, instrument-group buses, reverb and echo send/return, bus
// compressor, master fader and true-peak limiter. Owns the ParamEngine.
// Audio thread only: no allocation, no locks, no I/O (all buffers preallocated
// in prepare() for kMaxTracks tracks).
#include "analysis/Taps.h"
#include "dsp/ChannelStrip.h"
#include "dsp/Echo.h"
#include "dsp/FdnReverb.h"
#include "dsp/Limiter.h"
#include "dsp/ParamEngine.h"
#include <array>
#include <atomic>
#include <vector>

namespace im
{
struct TrackMeters
{
    std::atomic<float> inPeak { 0 }, inRms { 0 }, outPeak { 0 }, outRms { 0 }, compGr { 0 }, gateGr { 0 };
};

struct MasterMeters
{
    std::atomic<float> peakL { 0 }, peakR { 0 }, rms { 0 }, busGr { 0 }, limGr { 0 };
    std::array<std::atomic<float>, kNumGroupBuses> groupGr {}; // instrument-group bus compressors, max per block
    std::array<std::atomic<float>, kNumGroupBuses> groupGainDb {}; // bus output vs input energy, ~3 s average (comp + makeup + clip)
};

class MixEngine
{
public:
    ParamEngine params;
    std::array<TrackMeters, kMaxTracks> meters;
    // Instrument group per track (0 vocals .. 4 keys/other, -1 none), set from the track names on the message thread;
    // a track with a group is summed into that group's bus (compressor + peak clipper) instead of straight to the mix.
    std::array<std::atomic<int>, kMaxTracks> trackGroup {};

    MixEngine() { for (auto& g : trackGroup) g.store (-1); }
    MasterMeters masterMeters;

    void prepare (double sampleRate, int maxBlockSize);
    int maxBlock() const noexcept { return maxBlockSize; }
    int latencySamples() const noexcept { return limiter.latencySamples(); }
    void setTaps (AnalysisTaps* t) noexcept { taps = t; }

    // inL/inR: kMaxTracks pointers (nullptr = track not present). n <= maxBlock().
    void process (const float* const* inL, const float* const* inR, float* outL, float* outR, int n) noexcept;

    ChannelStrip& strip (int t) noexcept { return strips[(size_t) t]; }

    // A/B bypass: output the raw input sum (delayed by the reported latency so it stays
    // time-aligned), crossfaded over 20 ms. Optional slow RMS level match to the processed mix.
    // Analysis taps always see the processed mix, so the agent is unaffected.
    void setBypass (bool b) noexcept { bypass.store (b); }
    bool isBypassed() const noexcept { return bypass.load(); }
    void setBypassLevelMatch (bool m) noexcept { levelMatch.store (m); }
    double sampleRate() const noexcept { return fs; }

    // Static config/param builders (also used by UI curves).
    static StripConfig configFromParams (const ParamEngine& pe, int track0) noexcept;
    static StripBlockParams blockParamsFromParams (const ParamEngine& pe, int track0) noexcept;

private:
    void fillRamp (float* dst, float a, float b, int n) noexcept;
    void pushInterleaved (SpscRing<float>& ring, const float* l, const float* r, int n) noexcept;

    double fs = 48000.0;
    std::array<double, kNumGroupBuses> groupInE {}, groupOutE {};
    int maxBlockSize = 512;
    AnalysisTaps* taps = nullptr;

    std::array<ChannelStrip, kMaxTracks> strips;
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, kMaxTracks> audiblePower;
    std::array<bool, kMaxTracks> audible {};
    FdnReverb reverb;
    Echo echo;
    std::array<Compressor, kNumGroupBuses> groupComp;
    std::array<PeakClipper, kNumGroupBuses> groupClip;
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, kNumGroupBuses> groupOnPower;
    std::array<std::vector<float>, kNumGroupBuses> groupL, groupR;
    Compressor busComp;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> busOnPower, busMakeupDb, limOn;
    Limiter limiter;

    std::atomic<bool> bypass { false }, levelMatch { true };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassW;
    std::vector<float> dryL, dryR;      // dry-sum delay line (latency alignment)
    int dryMask = 0, dryPos = 0;
    double procPow = 1e-6, dryPow = 1e-6;
    float matchGain = 1.0f, powCoef = 0.0f, matchCoef = 0.0f;

    // scratch
    std::vector<float> gTrim, gWidth, gBalL, gBalR, gFader, gSend, gMute;
    std::vector<float> tOutL, tOutR, tSendL, tSendR;
    std::vector<float> sumL, sumR, sendSumL, sendSumR, wetL, wetR, tmp, limW, inter, echoSumL, echoSumR, gEcho;
};
} // namespace im
