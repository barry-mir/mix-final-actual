#pragma once
// Doubler: simulates a second take rather than copying the first one.
//
// A fixed-delay copy summed with the dry signal is a static comb filter - notches at fixed frequencies that the ear
// reads as a hollow filter, and that stay put when the mix is folded to mono. Continuously modulating the delay time
// produces the small pitch drift a real second performance has, and sweeps those notches so no frequency is
// persistently cancelled, which is what keeps it mono-compatible.
//
// Modulation is smoothed noise (a new random target each cycle, joined with a smoothstep), not a sine LFO: a periodic
// LFO gives regular pitch wobble, which is a chorus. The two channels run independent generators and different base
// delays, so they decorrelate. Base delay stays under ~30 ms or it stops being a double and becomes an echo.
// Fractional delay is read with 4-point Lagrange interpolation.
//
// Mixing: the doubles replace part of the dry signal at constant power (dry and doubles are uncorrelated, so a linear
// crossfade would dip the track by ~3 dB at mix 0.5 and the doubled track would just sound quieter). mix sets how far
// L and R come apart: their correlation is about 1 - 0.85 * mix (0.5 -> ~0.6, a clear double; 1 -> ~0.15, close to
// two takes panned left and right), with the dry signal still under the doubles for timing. Below ~150 Hz the track
// stays dry: the dry signal is split with a one-pole (low + high = input exactly, so mix 0 is a clean bypass) and only
// its high part is turned down, while the doubles go through a 24 dB/oct high-pass at the same frequency; the two
// roll-offs sum to about constant power through the crossover, so the low end stays centred and mono sums do not comb there.
//
// Refs: Dattorro, "Effect Design Part 2" (CCRMA); Smith, "Time-Varying Delay Effects" (PASP).
#include "dsp/Filters.h"
#include <array>
#include <cstdint>
#include <vector>

namespace im
{
struct DoublerParams
{
    float delayMs = 18.0f;  // base delay, both channels
    float spreadMs = 8.0f;  // L/R difference (L = delay - spread/2, R = delay + spread/2)
    float depthMs = 2.0f;   // peak modulation either side of the base delay
    float rateHz = 0.7f;    // how often a new random target is picked
    float mix = 0.0f;       // 0 = dry only .. 1 = mostly the two doubles (see above)
};

// Bounded, smooth, non-periodic modulation in [-1, 1].
class SmoothNoise
{
public:
    void reset (uint32_t seed) noexcept
    {
        rng = seed | 1u;
        phase = 0.0f;
        prev = next_ = 0.0f;
    }
    float next (float incrPerSample) noexcept
    {
        phase += incrPerSample;
        if (phase >= 1.0f)
        {
            phase -= (float) (int) phase;
            prev = next_;
            next_ = uniform();
        }
        const float t = phase, s = t * t * (3.0f - 2.0f * t); // smoothstep: zero slope at both ends, so no corners
        return prev + (next_ - prev) * s;
    }

private:
    float uniform() noexcept
    {
        rng = rng * 1664525u + 1013904223u;
        return (float) ((rng >> 8) & 0xFFFFFF) / 8388608.0f - 1.0f; // [-1, 1)
    }
    uint32_t rng = 1;
    float phase = 0, prev = 0, next_ = 0;
};

class Doubler
{
public:
    static constexpr float kMaxDelayMs = 60.0f;
    static constexpr float kMaxDepthMs = 10.0f;
    static constexpr double kCrossoverHz = 150.0; // below this the track stays dry

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setParams (const DoublerParams& p) noexcept { target = p; }

    // In place; dry is preserved when mix is 0 (the delay line keeps running so turning it up never clicks).
    void process (float* l, float* r, int n) noexcept;

    bool isActive() const noexcept { return target.mix > 1.0e-4f || cur.mix > 1.0e-4f; }

private:
    float read (int ch, float delaySamples) const noexcept;

    double fs = 48000.0;
    int size = 0, mask = 0;
    std::array<std::vector<float>, 2> buf;
    int writePos = 0;
    std::array<SmoothNoise, 2> lfo;
    DoublerParams target, cur;
    std::array<float, 2> delaySm { 0, 0 }; // one-pole smoothed delay, so moving the control does not click
    std::array<float, 2> lowDry { 0, 0 }; // one-pole low-pass of the dry signal per channel
    ButterworthCascade wetHp;            // 24 dB/oct high-pass on the doubles
    float xoA = 0.0f;                                      // crossover coefficient
    bool primed = false;
};
} // namespace im
