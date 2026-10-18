#pragma once
// Audio -> analysis thread transport. Full-rate (no decimation) interleaved
// stereo samples per track (pre- and post-strip) and for the master, plus
// per-block gain-reduction records. All lock-free SPSC; storage allocated in
// prepare() off the audio thread.
#include "core/Params.h"
#include "core/Spsc.h"
#include <array>
#include <atomic>

namespace im
{
struct GrRecord
{
    float compGrDb = 0, gateGrDb = 0;
    int numSamples = 0;
};

struct AnalysisTaps
{
    std::vector<SpscRing<float>> pre = std::vector<SpscRing<float>> ((size_t) maxTracks()), post = std::vector<SpscRing<float>> ((size_t) maxTracks());
    SpscRing<float> master, preMaster; // preMaster: the mix before the glue compressor and limiter
    std::vector<SpscRing<GrRecord>> gr = std::vector<SpscRing<GrRecord>> ((size_t) maxTracks());
    std::atomic<uint64_t> droppedSamples { 0 };
    double sampleRate = 48000.0;

    void prepare (double fs)
    {
        sampleRate = fs;
        const size_t cap = (size_t) (fs * 1.0) * 2; // 1 s of stereo per stream
        for (auto& r : pre) r.resize (cap);
        for (auto& r : post) r.resize (cap);
        master.resize (cap);
        preMaster.resize (cap);
        for (auto& r : gr) r.resize (8192);
    }
};
} // namespace im
