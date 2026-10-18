#pragma once
// Analysis thread: drains the full-rate SPSC taps into 3 s history windows
// and every 1 s (hop) computes a Snapshot of features for every present
// track (pre- and post-strip), gain-reduction stats, and the master (+LUFS).
#include "analysis/Features.h"
#include "analysis/Taps.h"
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace im
{
struct TrackSnapshot
{
    bool present = false;
    bool silent = true;      // pre-strip RMS < -60 dBFS over the window
    StereoFeatures pre, post;
    float compGrMean = 0, compGrMax = 0, gateGrMean = 0, gateGrMax = 0;
    float peakDb = -120;     // post-strip sample peak
    float levelAvgDb = -120; // post-strip power averaged since the last parameter change, minus the master's (dB); see Snapshot::avgSec
};

struct MasterSnapshot
{
    bool silent = true;
    StereoFeatures f;
    float cfPreDb = 0;       // crest factor of the mix before the glue compressor and limiter (this 3 s window)
    float prePeakDb = -120, preRmsDb = -120; // the same window's peak and level
    float ltCfPreDb = 0;     // whole-excerpt crest before the master chain: highest peak since the last parameter change
                             // over the energy-average level since then (rare peaks, not density); 0 = no average yet
    float lufs = -120;
    float peakDb = -120;
};

struct Snapshot
{
    int64_t seq = 0;
    double timeSec = 0;      // wall-clock (Time::getMillisecondCounterHiRes / 1000)
    double windowSec = 3.0;
    double avgSec = 0;       // audio covered by TrackSnapshot::levelAvgDb (0 = no average yet)
    std::array<TrackSnapshot, kMaxTracks> tracks;
    MasterSnapshot master;
};

class Analyzer : private juce::Thread
{
public:
    static constexpr double kWindowSec = 3.0;
    static constexpr double kHopSec = 1.0;
    static constexpr float kSilentDb = -60.0f;

    explicit Analyzer (AnalysisTaps& t);
    ~Analyzer() override;

    void start (double sampleRate, bool launchThread = true);
    void stop();

    void setPresentMask (TrackMask mask) noexcept { presentMask.store (mask); }
    // Any mix parameter changed: restart the long-term level average once the 3 s window holds only new audio.
    void noteParamChange() noexcept { changeSerial.fetch_add (1, std::memory_order_relaxed); }
    std::shared_ptr<const Snapshot> latest() const;

    // Deterministic, synchronous step (used by tests/offline): drain + compute if a hop elapsed.
    // Returns true if a new snapshot was produced.
    bool step();

private:
    void run() override;
    struct History
    {
        std::vector<float> l, r; // circular
        int pos = 0, filled = 0;
        void init (int n) { l.assign ((size_t) n, 0); r.assign ((size_t) n, 0); pos = filled = 0; }
        void push (const float* inter, int frames)
        {
            const int n = (int) l.size();
            for (int i = 0; i < frames; ++i)
            {
                l[(size_t) pos] = inter[2 * i];
                r[(size_t) pos] = inter[2 * i + 1];
                pos = pos + 1 == n ? 0 : pos + 1;
            }
            filled = std::min (n, filled + frames);
        }
        int linearize (std::vector<float>& outL, std::vector<float>& outR) const
        {
            const int n = (int) l.size();
            const int cnt = filled;
            int start = (pos - cnt + n) % n;
            for (int i = 0; i < cnt; ++i)
            {
                outL[(size_t) i] = l[(size_t) ((start + i) % n)];
                outR[(size_t) i] = r[(size_t) ((start + i) % n)];
            }
            return cnt;
        }
    };
    struct GrHistory
    {
        std::vector<GrRecord> recs;
        int pos = 0, filled = 0;
    };

    void drain();
    void compute();

    AnalysisTaps& taps;
    double fs = 48000.0;
    std::unique_ptr<FeatureExtractor> fx;
    std::array<History, kMaxTracks> pre, post;
    History master, preMaster;
    std::array<GrHistory, kMaxTracks> gr;
    std::vector<float> drainBuf, linL, linR;
    int64_t samplesSinceHop = 0;
    int64_t seq = 0;
    std::atomic<TrackMask> presentMask { 0 };
    // long-term level average (energy mean over snapshots since the last parameter change)
    std::atomic<uint32_t> changeSerial { 0 };
    uint32_t seenSerial = 0;
    int skipSnaps = 0, avgCount = 0;
    std::array<double, kMaxTracks> avgTrackPow {};
    double avgMasterPow = 0, avgPrePow = 0;
    float ltPrePeakDb = -120;
    mutable std::mutex snapMutex;
    std::shared_ptr<const Snapshot> snap;
    std::mutex stepMutex;
    bool running = false;
};
} // namespace im
