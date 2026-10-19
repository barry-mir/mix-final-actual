#include "analysis/Spectrum.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>

namespace im
{
const std::vector<float>& spectrumBandHz()
{
    static const std::vector<float> hz = [] {
        std::vector<float> v;
        for (int k = 0; k < 120; ++k) v.push_back ((float) (20.0 * std::pow (2.0, k / 12.0)));
        return v;
    }();
    return hz;
}

void spectrumDb (const float* x, int n, double fs, std::vector<float>& out)
{
    const auto& hz = spectrumBandHz();
    out.assign (hz.size(), -120.0f);
    if (n < 512 || (n & (n - 1)) != 0 || fs <= 0) return;
    const int order = (int) std::lround (std::log2 ((double) n));
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) n * 2, 0.0f);
    constexpr double kTwoPi = 6.283185307179586;
    for (int i = 0; i < n; ++i) buf[(size_t) i] = x[i] * (float) (0.5 - 0.5 * std::cos (kTwoPi * i / (n - 1)));
    fft.performFrequencyOnlyForwardTransform (buf.data(), true); // magnitudes in buf[0 .. n/2]
    // A full-scale sine with a Hann window peaks at n/4.
    const double ref = n / 4.0, binHz = fs / n;
    const int nb = n / 2;
    for (size_t k = 0; k < hz.size(); ++k)
    {
        const double lo = hz[k] / std::pow (2.0, 1.0 / 24.0), hi = hz[k] * std::pow (2.0, 1.0 / 24.0);
        int a = (int) std::ceil (lo / binHz), b = (int) std::floor (hi / binHz);
        double mag = 0;
        if (a <= b)
            for (int i = std::max (1, a); i <= std::min (nb, b); ++i) mag = std::max (mag, (double) buf[(size_t) i]);
        else // narrower than one bin (low bands): the nearest bin
            mag = buf[(size_t) std::clamp ((int) std::lround (hz[k] / binHz), 1, nb)];
        out[k] = (float) std::max (-120.0, 20.0 * std::log10 (std::max (mag / ref, 1e-9)));
    }
}
} // namespace im
