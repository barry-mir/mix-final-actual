#include "dsp/Doubler.h"
#include <algorithm>
#include <cmath>

namespace im
{
void Doubler::prepare (double sampleRate) noexcept
{
    fs = sampleRate;
    const int need = (int) std::ceil ((kMaxDelayMs + kMaxDepthMs) * 0.001 * fs) + 8;
    size = 1;
    while (size < need) size <<= 1; // power of two so the read wraps with a mask
    mask = size - 1;
    for (auto& b : buf) b.assign ((size_t) size, 0.0f);
    xoA = 1.0f - (float) std::exp (-2.0 * 3.141592653589793 * kCrossoverHz / fs);
    wetHp.design (true, fs, kCrossoverHz, 24);
    reset();
}

void Doubler::reset() noexcept
{
    for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f);
    writePos = 0;
    lfo[0].reset (0x9E3779B9u);
    lfo[1].reset (0x7F4A7C15u); // different seeds: the two channels must not drift together
    cur = target;
    primed = false;
    lowDry = { 0.0f, 0.0f };
    wetHp.reset();
}

// 4-point Lagrange: flat magnitude and close to linear phase, and unlike linear interpolation it does not
// lose high end as the fractional part moves - which matters here because the delay is moving constantly.
float Doubler::read (int ch, float delaySamples) const noexcept
{
    const float d = std::clamp (delaySamples, 2.0f, (float) (size - 4));
    const int i = (int) d;
    const float f = d - (float) i;
    const auto& b = buf[(size_t) ch];
    const int p = writePos - i;
    const float x0 = b[(size_t) ((p + 1) & mask)];
    const float x1 = b[(size_t) (p & mask)];
    const float x2 = b[(size_t) ((p - 1) & mask)];
    const float x3 = b[(size_t) ((p - 2) & mask)];
    const float c0 = -f * (f - 1.0f) * (f - 2.0f) / 6.0f;
    const float c1 = (f + 1.0f) * (f - 1.0f) * (f - 2.0f) / 2.0f;
    const float c2 = -(f + 1.0f) * f * (f - 2.0f) / 2.0f;
    const float c3 = (f + 1.0f) * f * (f - 1.0f) / 6.0f;
    return c0 * x0 + c1 * x1 + c2 * x2 + c3 * x3;
}

void Doubler::process (float* l, float* r, int n) noexcept
{
    if (size == 0) return;
    const float msToS = 0.001f * (float) fs;
    // per-block parameter ramp (the delay itself is smoothed per sample below)
    const float mix0 = cur.mix, dMix = (target.mix - cur.mix) / (float) std::max (1, n);
    const float depth0 = cur.depthMs, dDepth = (target.depthMs - cur.depthMs) / (float) std::max (1, n);
    cur = target;

    const float baseL = (target.delayMs - 0.5f * target.spreadMs) * msToS;
    const float baseR = (target.delayMs + 0.5f * target.spreadMs) * msToS;
    if (! primed)
    {
        delaySm[0] = baseL;
        delaySm[1] = baseR;
        primed = true;
    }
    // ~20 ms time constant: fast enough to follow a knob, slow enough that the pitch glide it causes is not a chirp
    const float sm = 1.0f - std::exp (-1.0f / (0.02f * (float) fs));
    const float incr = std::max (0.01f, target.rateHz) / (float) fs;

    for (int i = 0; i < n; ++i)
    {
        buf[0][(size_t) writePos] = l[i];
        buf[1][(size_t) writePos] = r[i];

        const float mix = mix0 + dMix * (float) i;
        const float depth = (depth0 + dDepth * (float) i) * msToS;
        delaySm[0] += sm * (baseL - delaySm[0]);
        delaySm[1] += sm * (baseR - delaySm[1]);

        const float wet[2] = { read (0, delaySm[0] + depth * lfo[0].next (incr)), read (1, delaySm[1] + depth * lfo[1].next (incr)) };
        // constant power: dry^2 + doubles^2 = 1, L/R correlation ~ 1 - 0.85 * mix
        const float w2 = 0.85f * std::clamp (mix, 0.0f, 1.0f), gDry = std::sqrt (1.0f - w2), gWet = std::sqrt (w2);
        float* io[2] = { l + i, r + i };
        for (int ch = 0; ch < 2; ++ch)
        {
            const float x = *io[ch];
            auto& lo = lowDry[(size_t) ch];
            lo += xoA * (x - lo);
            *io[ch] = lo + gDry * (x - lo) + gWet * wetHp.process (ch, wet[ch]);
        }

        writePos = (writePos + 1) & mask;
    }
}
} // namespace im
