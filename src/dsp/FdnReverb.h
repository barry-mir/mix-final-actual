#pragma once
// 8-line feedback delay network reverb.
// Jot & Chaigne, "Digital Delay Networks for Designing Artificial
// Reverberators", AES 1991: Hadamard feedback matrix (lossless, scaled 1/sqrt(8))
// and per-line one-pole absorption filters giving frequency-dependent decay:
//   k_p = 10^(-3 m_p / (T60(0) fs)),  H_p(z) = k_p (1 - a_p) / (1 - a_p z^-1),
//   a_p = (ln10 / 4) log10(k_p) (1 - 1/alpha^2),  alpha = T60(pi) / T60(0).
// See also Valimaki et al., "Fifty Years of Artificial Reverberation", IEEE TASLP 2012.
#include "dsp/Filters.h"
#include <array>
#include <vector>

namespace im
{
struct ReverbParams
{
    float predelayMs = 20, decayS = 1.8f, size = 0.5f, hfDampHz = 8000, lowCutHz = 200, highCutHz = 12000,
          width = 1.0f, erLevelDb = -12.0f;
};

class FdnReverb
{
public:
    static constexpr int kLines = 8;
    static constexpr int kErTaps = 6;

    void prepare (double sampleRate, int maxBlock);
    void reset();
    void setParams (const ReverbParams& p) noexcept; // call once per block
    // 100% wet. Input = summed sends, output = wet stereo.
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;

    // For tests: delay length (samples) of line i and the HF ratio alpha.
    double lineDelay (int i) const noexcept { return delayLen[(size_t) i]; }
    static float alphaForDamping (float hfDampHz) noexcept;

private:
    struct Delay
    {
        std::vector<float> buf;
        int mask = 0, w = 0;
        void init (int maxLen)
        {
            int c = 1;
            while (c < maxLen + 4) c <<= 1;
            buf.assign ((size_t) c, 0.0f);
            mask = c - 1;
            w = 0;
        }
        void clear() { std::fill (buf.begin(), buf.end(), 0.0f); }
        inline void push (float x) noexcept { buf[(size_t) w] = x; w = (w + 1) & mask; }
        // read d samples back (d >= 1), linear interpolation
        inline float read (float d) const noexcept
        {
            const int di = (int) d;
            const float fr = d - (float) di;
            const float a = buf[(size_t) ((w - di) & mask)];
            const float b = buf[(size_t) ((w - di - 1) & mask)];
            return a + fr * (b - a);
        }
    };

    double fs = 48000.0;
    std::array<Delay, kLines> lines;
    Delay preL, preR;
    std::array<double, kLines> delayLen {}, targetLen {}; // double: float glide stalls at fractional lengths
    std::array<float, kLines> absB {}, absA {}, absState {};
    double predelay = 0, targetPredelay = 0;
    StereoBiquad lowCut, highCut;
    std::array<float, kErTaps> erTimes {}, erGains {};
    float erLevel = 0.25f, width = 1.0f;
    double lenSmooth = 0.0; // per-sample one-pole coefficient for delay-length changes
};
} // namespace im
