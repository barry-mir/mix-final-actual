#pragma once
// Audio features ported from Diff-MST (Vanka et al., ISMIR 2024,
// github.com/sai-soum/Diff-MST, mst/loss.py + mst/filter.py):
//   compute_rms            rms[c]  = sqrt(max(mean(x_c^2), 1e-8))
//   compute_crest_factor   cf[c]   = 20 log10(max(max|x_c| / max(rms[c],1e-8), 1e-8))
//   compute_stereo_width   sw      = mean((L-R)^2) / max(mean((L+R)^2), 1e-8)
//   compute_stereo_imbalance si    = (E_R - E_L) / max(E_R + E_L, 1e-8)
//   compute_barkspectrum   (mode "mid-side": mid = L+R, side = L-R)
//       STFT n_fft=32768, hop=8192, periodic Hann, center=True (reflect pad),
//       |X| averaged over frames, 24-band triangular Traunmuller bark filterbank
//       (f_min 20, f_max 20000, incl. the reference's bark-correction quirks),
//       then natural log(X + 1e-8).  Output [mid(24), side(24)].
#include <array>
#include <memory>
#include <vector>

namespace juce::dsp { class FFT; }

namespace im
{
constexpr int kBarkBands = 24;
constexpr int kAggBands = 8;
extern const char* const kAggBandNames[kAggBands]; // sub low lmid mid umid pres bril air

struct StereoFeatures
{
    // Exact Diff-MST quantities
    std::array<float, 2> rms {};  // linear
    std::array<float, 2> cf {};   // dB
    float sw = 0;                 // ratio
    float si = 0;                 // ratio
    std::array<float, kBarkBands> barkMid {}, barkSide {}; // ln magnitude
    // Derived (for UI / prompt)
    float rmsDb = -120;   // 10 log10 of mean channel power
    float cfDb = 0;       // mean of channel crest factors
    float peakDb = -120;  // sample peak dBFS
    std::array<float, kAggBands> bands {}; // dB relative to total band energy (≈ track RMS)
};

class FeatureExtractor
{
public:
    static constexpr int kFftOrder = 15; // 32768
    static constexpr int kFftSize = 1 << kFftOrder;
    static constexpr int kHop = kFftSize / 4;

    explicit FeatureExtractor (double sampleRate);
    ~FeatureExtractor();

    // Not realtime-safe (allocates nothing after construction for n <= maxLen given at first call).
    StereoFeatures compute (const float* L, const float* R, int n);

    // Filterbank access for verification
    const std::vector<std::array<float, kBarkBands>>& filterbank() const { return fb; }
    const std::array<double, kBarkBands + 2>& bandEdgesHz() const { return fPts; }
    // Aggregation weights [group][band]
    const std::array<std::array<float, kBarkBands>, kAggBands>& aggWeights() const { return agg; }

    static std::array<float, kAggBands> aggregate (const std::array<float, kBarkBands>& barkMid,
                                                   const std::array<std::array<float, kBarkBands>, kAggBands>& w);

private:
    void barkSpectrum (const float* x, int n, std::array<float, kBarkBands>& out);

    double fs;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, frame, padded, magAcc, mid, side;
    std::vector<std::array<float, kBarkBands>> fb; // [freqBin][band]
    std::array<double, kBarkBands + 2> fPts {};
    std::array<std::array<float, kBarkBands>, kAggBands> agg {};
};

// Diff-MST bark helpers, exposed for tests.
double diffmstHzToBark (double f);
} // namespace im
