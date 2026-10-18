#include "dsp/Echo.h"
#include <algorithm>
#include <cmath>

namespace im
{
void Echo::prepare (double sampleRate)
{
    fs = sampleRate;
    const int need = (int) std::ceil (kMaxTimeMs * 0.001 * fs) + 8;
    size = 1;
    while (size < need) size <<= 1;
    mask = size - 1;
    for (auto& b : buf) b.assign ((size_t) size, 0.0f);
    smCoef = 1.0f - (float) std::exp (-1.0 / (0.08 * fs));
    reset();
}

void Echo::reset()
{
    for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f);
    writePos = 0;
    lowCut.reset();
    highCut.reset();
    primed = false;
}

void Echo::setParams (const EchoParams& p) noexcept
{
    const float ms = 0.001f * (float) fs;
    target = { std::clamp (p.timeLMs, 1.0f, kMaxTimeMs) * ms, std::clamp (p.timeRMs, 1.0f, kMaxTimeMs) * ms };
    feedback = std::clamp (p.feedback, 0.0f, 0.9f);
    lowCut.c = BiquadCoeffs::highpass (fs, safeFreq (fs, p.lowCutHz), 0.7071);
    highCut.c = BiquadCoeffs::lowpass (fs, safeFreq (fs, p.highCutHz), 0.7071);
    if (! primed)
    {
        timeSm = target;
        primed = true;
    }
}

float Echo::read (int ch, float d) const noexcept
{
    d = std::clamp (d, 1.0f, (float) (size - 2));
    const int i = (int) d;
    const float f = d - (float) i;
    const auto& b = buf[(size_t) ch];
    return b[(size_t) ((writePos - i) & mask)] * (1.0f - f) + b[(size_t) ((writePos - i - 1) & mask)] * f;
}

void Echo::process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
{
    if (size == 0) return;
    for (int i = 0; i < n; ++i)
    {
        for (int ch = 0; ch < 2; ++ch) timeSm[(size_t) ch] += smCoef * (target[(size_t) ch] - timeSm[(size_t) ch]);
        const float yl = read (0, timeSm[0]), yr = read (1, timeSm[1]);
        // input + feedback, filtered before it enters the line: every repeat passes the filters once more
        float wl = inL[i] + feedback * yl, wr = inR[i] + feedback * yr;
        wl = highCut.process (0, lowCut.process (0, wl));
        wr = highCut.process (1, lowCut.process (1, wr));
        buf[0][(size_t) writePos] = wl;
        buf[1][(size_t) writePos] = wr;
        writePos = (writePos + 1) & mask;
        outL[i] = yl;
        outR[i] = yr;
    }
}
} // namespace im
