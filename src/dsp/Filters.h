#pragma once
// Biquad filters per the RBJ Audio EQ Cookbook (Robert Bristow-Johnson),
// plus Butterworth cascades for HPF/LPF slopes (12/18/24 dB/oct).
#include <array>
#include <cmath>

namespace im
{
struct BiquadCoeffs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; // normalised (a0 == 1)

    static BiquadCoeffs identity() { return {}; }
    // RBJ cookbook designs. f0 in Hz, gainDb for peak/shelf, q for bandwidth.
    static BiquadCoeffs lowpass (double fs, double f0, double q);
    static BiquadCoeffs highpass (double fs, double f0, double q);
    static BiquadCoeffs peaking (double fs, double f0, double q, double gainDb);
    static BiquadCoeffs lowShelf (double fs, double f0, double gainDb, double shelfSlope = 1.0);
    static BiquadCoeffs highShelf (double fs, double f0, double gainDb, double shelfSlope = 1.0);
    // First-order sections (bilinear transform with prewarping).
    static BiquadCoeffs lowpass1 (double fs, double f0);
    static BiquadCoeffs highpass1 (double fs, double f0);

    // |H(e^jw)| at frequency f (Hz) for analysis/UI curves.
    double magnitude (double fs, double f) const;
};

// Transposed direct form II, stereo, double state.
struct StereoBiquad
{
    BiquadCoeffs c;
    double z1[2] {}, z2[2] {};

    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    inline float process (int ch, float xf) noexcept
    {
        const double x = xf;
        const double y = c.b0 * x + z1[ch];
        z1[ch] = c.b1 * x - c.a1 * y + z2[ch];
        z2[ch] = c.b2 * x - c.a2 * y;
        return (float) y;
    }
};

// Butterworth HPF/LPF built from cascaded sections. slopeDb in {12,18,24}.
struct ButterworthCascade
{
    static constexpr int kMaxSections = 2;
    std::array<StereoBiquad, kMaxSections> sec;
    int numSections = 1;

    void reset()
    {
        for (auto& s : sec) s.reset();
    }
    void design (bool highpass, double fs, double f0, int slopeDb);
    inline float process (int ch, float x) noexcept
    {
        for (int i = 0; i < numSections; ++i) x = sec[(size_t) i].process (ch, x);
        return x;
    }
    double magnitude (double fs, double f) const;
};

// Clamp a design frequency to a safe range below Nyquist.
inline double safeFreq (double fs, double f) { return std::fmin (std::fmax (f, 5.0), fs * 0.49); }
} // namespace im
