#pragma once
// The complete realtime mixer: maxTracks() channel strips, instrument-group buses, reverb and echo send/return, bus
// compressor, master fader and true-peak limiter. Owns the ParamEngine.
// Audio thread only: no allocation, no locks, no I/O (all buffers preallocated
// in prepare() for maxTracks() tracks).
#include "analysis/Loudness.h"
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
    std::array<std::atomic<float>, kNumGroupBuses> groupPeak {}, groupRms {}; // bus output, this block (linear)
    std::atomic<float> reverbPeak { 0 }, echoPeak { 0 };                  // FX returns, this block (linear)
    std::atomic<float> lufsM { -120 };                                     // momentary loudness of the output (what plays)
};

// The last kSize samples of one mono signal, written by the audio thread and read by another thread (the page's
// spectrum view). A read can see a few samples from two different blocks; for a display that is fine.
struct ScopeRing
{
    static constexpr int kSize = 8192;
    std::vector<std::atomic<float>> buf = std::vector<std::atomic<float>> ((size_t) kSize);
    std::atomic<uint32_t> written { 0 };
    void push (const float* l, const float* r, int n) noexcept
    {
        uint32_t w = written.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i, ++w) buf[w & (kSize - 1)].store (0.5f * (l[i] + r[i]), std::memory_order_relaxed);
        written.store (w, std::memory_order_release);
    }
    // The newest n (<= kSize) samples, oldest first.
    void latest (float* out, int n) const noexcept
    {
        const uint32_t w = written.load (std::memory_order_acquire);
        for (int i = 0; i < n; ++i) out[i] = buf[(w - (uint32_t) n + (uint32_t) i) & (kSize - 1)].load (std::memory_order_relaxed);
    }
};

class MixEngine
{
public:
    ParamEngine params;
    std::vector<TrackMeters> meters = std::vector<TrackMeters> ((size_t) maxTracks());
    // Instrument group per track (0 vocals .. 4 keys/other, -1 none), set from the track names on the message thread;
    // a track with a group is summed into that group's bus (compressor + peak clipper) instead of straight to the mix.
    std::vector<std::atomic<int>> trackGroup = std::vector<std::atomic<int>> ((size_t) maxTracks());

    MixEngine() { for (auto& g : trackGroup) g.store (-1); }
    MasterMeters masterMeters;

    // Extra gain before the limiter, outside the mix settings: master to a delivery loudness (0 = as mixed). Glides
    // over 1 s when it changes while playing; prepare() jumps to it (offline renders).
    std::atomic<float> deliveryGainDb { 0.0f };
    // While mastering, the limiter is on whatever its parameter says (export and live playback alike).
    std::atomic<bool> forceLimiter { false };
    // Live playback with a delivery gain: the analysis tap gets the mix as mixed (no delivery gain, the limiter as its
    // parameter says) through a second limiter, so the agent's measurements do not depend on the playback loudness.
    std::atomic<bool> separateAnalysis { false };
    // The page's spectrum view: the track (0..) or the master (-1) whose input (pre) and output (post) are copied to
    // the scope rings; -2 = none. Master: pre = the mix before the glue compressor and limiter, post = the output.
    std::atomic<int> scopeTrack { -2 };
    ScopeRing scopePre, scopePost;

    void prepare (double sampleRate, int maxBlockSize);
    int maxBlock() const noexcept { return maxBlockSize; }
    int latencySamples() const noexcept { return limiter.latencySamples(); }
    void setTaps (AnalysisTaps* t) noexcept { taps = t; }

    // inL/inR: maxTracks() pointers (nullptr = track not present). n <= maxBlock().
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

    std::vector<ChannelStrip> strips = std::vector<ChannelStrip> ((size_t) maxTracks());
    std::vector<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>> audiblePower = std::vector<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>> ((size_t) maxTracks());
    std::vector<bool> audible = std::vector<bool> ((size_t) maxTracks());
    FdnReverb reverb;
    Echo echo;
    std::array<Compressor, kNumGroupBuses> groupComp;
    std::array<PeakClipper, kNumGroupBuses> groupClip;
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, kNumGroupBuses> groupOnPower;
    std::array<std::vector<float>, kNumGroupBuses> groupL, groupR;
    Compressor busComp;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> busOnPower, busMakeupDb, limOn;
    Limiter limiter, analysisLimiter;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> deliverySm, anaLimOn;
    std::vector<float> anaL, anaR, anaW;
    MomentaryLoudness momentary;

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
