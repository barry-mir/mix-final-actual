#include "analysis/Features.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>

namespace im
{
const char* const kAggBandNames[kAggBands] = { "sub", "low", "lmid", "mid", "umid", "pres", "bril", "air" };

namespace
{
constexpr double kAggEdges[kAggBands + 1] = { 0, 60, 250, 500, 2000, 4000, 6000, 12000, 1.0e9 };
} // namespace

// mst/filter.py: _hz_to_bark (traunmuller, with correction)
double diffmstHzToBark (double f)
{
    double b = ((26.81 * f) / (1960.0 + f)) - 0.53;
    if (b < 2) b += 0.15 * (2 - b);
    else if (b > 20.1) b += 0.22 * (b - 20.1);
    return b;
}

FeatureExtractor::FeatureExtractor (double sampleRate) : fs (sampleRate)
{
    fft = std::make_unique<juce::dsp::FFT> (kFftOrder);
    window.resize ((size_t) kFftSize);
    for (int i = 0; i < kFftSize; ++i) // torch.hann_window (periodic=True)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / kFftSize));
    frame.assign ((size_t) kFftSize * 2, 0.0f);
    const int nFreqs = kFftSize / 2 + 1;
    magAcc.assign ((size_t) nFreqs, 0.0f);

    // mst/filter.py: barkscale_fbanks(n_freqs, 20, 20000, 24, sample_rate)
    std::array<double, kBarkBands + 2> m {};
    const double mMin = diffmstHzToBark (20.0), mMax = diffmstHzToBark (20000.0);
    for (int i = 0; i < kBarkBands + 2; ++i)
        m[(size_t) i] = (double) (float) (mMin + (mMax - mMin) * i / (kBarkBands + 1)); // torch.linspace (float32)
    // _bark_to_hz: `if any(barks < 2): ... elif any(barks > 20.1): ...` -> only the low correction applies
    bool anyLow = false, anyHigh = false;
    for (auto v : m) { anyLow |= v < 2; anyHigh |= v > 20.1; }
    for (auto& v : m)
    {
        if (anyLow) { if (v < 2) v = (v - 0.3) / 0.85; }
        else if (anyHigh) { if (v > 20.1) v = (v + 4.422) / 1.22; }
    }
    for (int i = 0; i < kBarkBands + 2; ++i)
        fPts[(size_t) i] = 1960.0 * ((m[(size_t) i] + 0.53) / (26.28 - m[(size_t) i]));

    const double nyq = (double) ((int) fs / 2); // sample_rate // 2
    fb.assign ((size_t) nFreqs, {});
    for (int k = 0; k < nFreqs; ++k)
    {
        const double f = nyq * k / (nFreqs - 1);
        for (int b = 0; b < kBarkBands; ++b)
        {
            const double down = -(fPts[(size_t) b] - f) / (fPts[(size_t) b + 1] - fPts[(size_t) b]);
            const double up = (fPts[(size_t) b + 2] - f) / (fPts[(size_t) b + 2] - fPts[(size_t) b + 1]);
            fb[(size_t) k][(size_t) b] = (float) std::max (0.0, std::min (down, up));
        }
    }

    // Aggregation weights: share of each bark triangle's area falling in each prompt band.
    for (int b = 0; b < kBarkBands; ++b)
    {
        double total = 0;
        std::array<double, kAggBands> part {};
        for (int k = 0; k < nFreqs; ++k)
        {
            const double f = nyq * k / (nFreqs - 1);
            const double w = fb[(size_t) k][(size_t) b];
            total += w;
            for (int g = 0; g < kAggBands; ++g)
                if (f >= kAggEdges[g] && f < kAggEdges[g + 1]) part[(size_t) g] += w;
        }
        for (int g = 0; g < kAggBands; ++g)
            agg[(size_t) g][(size_t) b] = total > 0 ? (float) (part[(size_t) g] / total) : 0.0f;
    }
}

FeatureExtractor::~FeatureExtractor() = default;

std::array<float, kAggBands> FeatureExtractor::aggregate (const std::array<float, kBarkBands>& barkMid,
                                                         const std::array<std::array<float, kBarkBands>, kAggBands>& w)
{
    std::array<double, kAggBands> e {};
    double total = 0;
    for (int g = 0; g < kAggBands; ++g)
    {
        for (int b = 0; b < kBarkBands; ++b)
            e[(size_t) g] += (double) w[(size_t) g][(size_t) b] * std::exp (2.0 * (double) barkMid[(size_t) b]);
        total += e[(size_t) g];
    }
    std::array<float, kAggBands> out {};
    for (int g = 0; g < kAggBands; ++g)
        out[(size_t) g] = (float) (10.0 * std::log10 (std::max (e[(size_t) g] / std::max (total, 1e-30), 1e-12)));
    return out;
}

void FeatureExtractor::barkSpectrum (const float* x, int n, std::array<float, kBarkBands>& out)
{
    // torch.stft(center=True, pad_mode="reflect")
    const int pad = kFftSize / 2;
    const int total = n + 2 * pad;
    if ((int) padded.size() < total) padded.resize ((size_t) total);
    for (int j = 0; j < total; ++j)
    {
        int i = j - pad;
        if (i < 0) i = -i;
        if (i >= n) i = 2 * (n - 1) - i;
        i = std::clamp (i, 0, n - 1);
        padded[(size_t) j] = x[i];
    }
    const int frames = 1 + n / kHop;
    const int nFreqs = kFftSize / 2 + 1;
    std::fill (magAcc.begin(), magAcc.end(), 0.0f);
    for (int f = 0; f < frames; ++f)
    {
        const float* src = padded.data() + (size_t) f * kHop;
        for (int i = 0; i < kFftSize; ++i) frame[(size_t) i] = src[i] * window[(size_t) i];
        std::fill (frame.begin() + kFftSize, frame.end(), 0.0f);
        fft->performFrequencyOnlyForwardTransform (frame.data(), true);
        for (int k = 0; k < nFreqs; ++k) magAcc[(size_t) k] += frame[(size_t) k];
    }
    std::array<double, kBarkBands> acc {};
    for (int k = 0; k < nFreqs; ++k)
    {
        const double mk = (double) magAcc[(size_t) k] / frames;
        if (mk == 0.0) continue;
        for (int b = 0; b < kBarkBands; ++b) acc[(size_t) b] += (double) fb[(size_t) k][(size_t) b] * mk;
    }
    for (int b = 0; b < kBarkBands; ++b) out[(size_t) b] = (float) std::log (acc[(size_t) b] + 1e-8);
}

StereoFeatures FeatureExtractor::compute (const float* L, const float* R, int n)
{
    StereoFeatures F;
    if (n <= 0) return F;
    double eL = 0, eR = 0, eSum = 0, eDiff = 0;
    float pkL = 0, pkR = 0;
    if ((int) mid.size() < n)
    {
        mid.resize ((size_t) n);
        side.resize ((size_t) n);
    }
    for (int i = 0; i < n; ++i)
    {
        const double l = L[i], r = R[i];
        eL += l * l;
        eR += r * r;
        const double s = l + r, d = l - r;
        eSum += s * s;
        eDiff += d * d;
        pkL = std::max (pkL, std::fabs (L[i]));
        pkR = std::max (pkR, std::fabs (R[i]));
        mid[(size_t) i] = (float) s;
        side[(size_t) i] = (float) d;
    }
    eL /= n;
    eR /= n;
    eSum /= n;
    eDiff /= n;
    F.rms[0] = (float) std::sqrt (std::max (eL, 1e-8));
    F.rms[1] = (float) std::sqrt (std::max (eR, 1e-8));
    F.cf[0] = (float) (20.0 * std::log10 (std::max ((double) pkL / std::max ((double) F.rms[0], 1e-8), 1e-8)));
    F.cf[1] = (float) (20.0 * std::log10 (std::max ((double) pkR / std::max ((double) F.rms[1], 1e-8), 1e-8)));
    F.sw = (float) (eDiff / std::max (eSum, 1e-8));
    F.si = (float) ((eR - eL) / std::max (eR + eL, 1e-8));

    barkSpectrum (mid.data(), n, F.barkMid);
    barkSpectrum (side.data(), n, F.barkSide);

    F.rmsDb = (float) (10.0 * std::log10 (std::max (0.5 * (eL + eR), 1e-12)));
    // Average only the channels that carry signal. A hard-panned track has one silent channel whose crest factor
    // floors at -160 dB, and averaging that in made the pair's reading negative (impossible: peak >= rms).
    {
        const bool okL = pkL > 1.0e-7f, okR = pkR > 1.0e-7f;
        F.cfDb = okL && okR ? 0.5f * (F.cf[0] + F.cf[1]) : okL ? F.cf[0] : okR ? F.cf[1] : 0.0f;
    }
    F.peakDb = 20.0f * std::log10 (std::max (std::max (pkL, pkR), 1e-6f));
    F.bands = aggregate (F.barkMid, agg);
    return F;
}
} // namespace im
