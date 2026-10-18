#include "dsp/FdnReverb.h"
#include <algorithm>
#include <cmath>

namespace im
{
namespace
{
// Mutually prime-ish base delays (ms) for size = 0.5.
constexpr std::array<float, FdnReverb::kLines> kBaseMs { 29.71f, 37.11f, 41.13f, 43.73f, 53.03f, 59.93f, 67.73f, 73.13f };
constexpr std::array<float, FdnReverb::kErTaps> kErMs { 7.1f, 11.3f, 17.9f, 23.3f, 31.7f, 41.9f };
constexpr std::array<float, FdnReverb::kErTaps> kErGain { 0.85f, -0.7f, 0.6f, -0.5f, 0.42f, -0.35f };
constexpr float kMaxSizeScale = 1.6f;

inline float sizeScale (float size) { return 0.4f + 1.2f * std::clamp (size, 0.0f, 1.0f); }

// In-place fast Walsh-Hadamard transform of length 8, scaled to be orthonormal.
inline void hadamard8 (float* x) noexcept
{
    for (int h = 1; h < 8; h <<= 1)
        for (int i = 0; i < 8; i += h << 1)
            for (int j = i; j < i + h; ++j)
            {
                const float a = x[j], b = x[j + h];
                x[j] = a + b;
                x[j + h] = a - b;
            }
    constexpr float s = 0.35355339059f; // 1/sqrt(8)
    for (int i = 0; i < 8; ++i) x[i] *= s;
}
} // namespace

float FdnReverb::alphaForDamping (float hfDampHz) noexcept
{
    // Map HF damping frequency to T60(pi)/T60(0): 20 kHz -> 1 (no damping), 1 kHz -> ~0.22.
    return std::clamp (std::sqrt (hfDampHz / 20000.0f), 0.05f, 1.0f);
}

void FdnReverb::prepare (double sampleRate, int)
{
    fs = sampleRate;
    const int maxLine = (int) std::ceil (0.001 * kBaseMs.back() * kMaxSizeScale * fs) + 8;
    for (auto& l : lines) l.init (maxLine);
    const int maxPre = (int) std::ceil (0.001 * (200.0 + kErMs.back() * kMaxSizeScale) * fs) + 8;
    preL.init (maxPre);
    preR.init (maxPre);
    lenSmooth = 1.0 - std::exp (-1.0 / (0.05 * fs)); // 50 ms glide for size changes
    ReverbParams p;
    setParams (p);
    delayLen = targetLen;
    predelay = targetPredelay;
    reset();
}

void FdnReverb::reset()
{
    for (auto& l : lines) l.clear();
    preL.clear();
    preR.clear();
    absState.fill (0.0f);
    lowCut.reset();
    highCut.reset();
}

void FdnReverb::setParams (const ReverbParams& p) noexcept
{
    const float sc = sizeScale (p.size);
    const float alpha = alphaForDamping (p.hfDampHz);
    const float t60 = std::max (0.05f, p.decayS);
    for (size_t i = 0; i < (size_t) kLines; ++i)
    {
        targetLen[i] = std::round (0.001 * kBaseMs[i] * sc * fs); // integer: no interpolation loss in the loop
        // Absorption computed for the target length (decay stays consistent while gliding).
        const float m = (float) targetLen[i];
        const float k = std::pow (10.0f, -3.0f * m / (t60 * (float) fs));
        const float a = std::log (10.0f) / 4.0f * std::log10 (k) * (1.0f - 1.0f / (alpha * alpha));
        absA[i] = std::clamp (a, 0.0f, 0.99f);
        absB[i] = k * (1.0f - absA[i]);
    }
    for (size_t t = 0; t < (size_t) kErTaps; ++t)
    {
        erTimes[t] = (float) (0.001 * kErMs[t] * sc * fs);
        erGains[t] = kErGain[t];
    }
    targetPredelay = 0.001 * p.predelayMs * fs;
    erLevel = p.erLevelDb <= -90.0f ? 0.0f : std::pow (10.0f, p.erLevelDb * 0.05f);
    width = std::clamp (p.width, 0.0f, 1.0f);
    lowCut.c = BiquadCoeffs::highpass (fs, p.lowCutHz, 0.7071);
    highCut.c = BiquadCoeffs::lowpass (fs, p.highCutHz, 0.7071);
}

void FdnReverb::process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
{
    float x[kLines];
    for (int s = 0; s < n; ++s)
    {
        // Glide delay lengths / pre-delay to avoid zipper noise on size changes.
        for (size_t i = 0; i < (size_t) kLines; ++i)
        {
            const double d = targetLen[i] - delayLen[i];
            delayLen[i] = std::fabs (d) < 0.01 ? targetLen[i] : delayLen[i] + lenSmooth * d; // settle on an integer length
        }
        predelay += lenSmooth * (targetPredelay - predelay);

        const float l = highCut.process (0, lowCut.process (0, inL[s]));
        const float r = highCut.process (1, lowCut.process (1, inR[s]));
        preL.push (l);
        preR.push (r);
        const float pd = (float) std::max (1.0, predelay);
        const float dl = preL.read (pd), dr = preR.read (pd);

        // Early reflections: sparse taps after the pre-delay.
        float erL = 0, erR = 0;
        for (size_t t = 0; t < (size_t) kErTaps; ++t)
        {
            erL += erGains[t] * preL.read (pd + erTimes[t]);
            erR += erGains[t] * preR.read (pd + erTimes[t] * 1.07f);
        }

        // FDN: read, absorb, mix, write.
        for (size_t i = 0; i < (size_t) kLines; ++i)
        {
            const float y = lines[i].read ((float) std::max (1.0, delayLen[i] - 1.0));
            absState[i] = absB[i] * y + absA[i] * absState[i];
            x[i] = absState[i];
        }
        float tailL = 0, tailR = 0;
        for (size_t i = 0; i < (size_t) kLines; i += 2)
        {
            tailL += (i & 2 ? -1.0f : 1.0f) * x[i];
            tailR += (i & 2 ? -1.0f : 1.0f) * x[i + 1];
        }
        hadamard8 (x);
        for (size_t i = 0; i < (size_t) kLines; ++i)
        {
            const float in = (i & 1) ? dr : dl;
            lines[i].push (x[i] + in * 0.5f);
        }

        float wl = 0.5f * tailL + erLevel * erL * 0.5f;
        float wr = 0.5f * tailR + erLevel * erR * 0.5f;
        const float m = 0.5f * (wl + wr), sd = 0.5f * (wl - wr) * width;
        outL[s] = m + sd;
        outR[s] = m - sd;
    }
}
} // namespace im
