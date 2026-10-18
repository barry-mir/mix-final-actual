#include "dsp/Filters.h"
#include <complex>

namespace im
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

BiquadCoeffs normalise (double b0, double b1, double b2, double a0, double a1, double a2)
{
    BiquadCoeffs c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}
} // namespace

BiquadCoeffs BiquadCoeffs::lowpass (double fs, double f0, double q)
{
    const double w0 = 2 * kPi * safeFreq (fs, f0) / fs, cw = std::cos (w0), alpha = std::sin (w0) / (2 * q);
    return normalise ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}

BiquadCoeffs BiquadCoeffs::highpass (double fs, double f0, double q)
{
    const double w0 = 2 * kPi * safeFreq (fs, f0) / fs, cw = std::cos (w0), alpha = std::sin (w0) / (2 * q);
    return normalise ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}

BiquadCoeffs BiquadCoeffs::peaking (double fs, double f0, double q, double gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0 = 2 * kPi * safeFreq (fs, f0) / fs, cw = std::cos (w0), alpha = std::sin (w0) / (2 * q);
    return normalise (1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
}

BiquadCoeffs BiquadCoeffs::lowShelf (double fs, double f0, double gainDb, double S)
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0 = 2 * kPi * safeFreq (fs, f0) / fs, cw = std::cos (w0), sw = std::sin (w0);
    const double alpha = sw / 2 * std::sqrt ((A + 1 / A) * (1 / S - 1) + 2);
    const double t = 2 * std::sqrt (A) * alpha;
    return normalise (A * ((A + 1) - (A - 1) * cw + t),
                      2 * A * ((A - 1) - (A + 1) * cw),
                      A * ((A + 1) - (A - 1) * cw - t),
                      (A + 1) + (A - 1) * cw + t,
                      -2 * ((A - 1) + (A + 1) * cw),
                      (A + 1) + (A - 1) * cw - t);
}

BiquadCoeffs BiquadCoeffs::highShelf (double fs, double f0, double gainDb, double S)
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0 = 2 * kPi * safeFreq (fs, f0) / fs, cw = std::cos (w0), sw = std::sin (w0);
    const double alpha = sw / 2 * std::sqrt ((A + 1 / A) * (1 / S - 1) + 2);
    const double t = 2 * std::sqrt (A) * alpha;
    return normalise (A * ((A + 1) + (A - 1) * cw + t),
                      -2 * A * ((A - 1) + (A + 1) * cw),
                      A * ((A + 1) + (A - 1) * cw - t),
                      (A + 1) - (A - 1) * cw + t,
                      2 * ((A - 1) - (A + 1) * cw),
                      (A + 1) - (A - 1) * cw - t);
}

BiquadCoeffs BiquadCoeffs::lowpass1 (double fs, double f0)
{
    const double K = std::tan (kPi * safeFreq (fs, f0) / fs);
    return normalise (K, K, 0, K + 1, K - 1, 0);
}

BiquadCoeffs BiquadCoeffs::highpass1 (double fs, double f0)
{
    const double K = std::tan (kPi * safeFreq (fs, f0) / fs);
    return normalise (1, -1, 0, K + 1, K - 1, 0);
}

double BiquadCoeffs::magnitude (double fs, double f) const
{
    const double w = 2 * kPi * f / fs;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
    const auto num = b0 + b1 * z1 + b2 * z2;
    const auto den = 1.0 + a1 * z1 + a2 * z2;
    return std::abs (num / den);
}

void ButterworthCascade::design (bool hp, double fs, double f0, int slopeDb)
{
    // Butterworth pole Qs: 2nd order: 0.7071; 3rd: 1st-order + Q=1.0; 4th: 0.5412, 1.3066
    if (slopeDb >= 24)
    {
        numSections = 2;
        sec[0].c = hp ? BiquadCoeffs::highpass (fs, f0, 0.54119610) : BiquadCoeffs::lowpass (fs, f0, 0.54119610);
        sec[1].c = hp ? BiquadCoeffs::highpass (fs, f0, 1.30656296) : BiquadCoeffs::lowpass (fs, f0, 1.30656296);
    }
    else if (slopeDb >= 18)
    {
        numSections = 2;
        sec[0].c = hp ? BiquadCoeffs::highpass1 (fs, f0) : BiquadCoeffs::lowpass1 (fs, f0);
        sec[1].c = hp ? BiquadCoeffs::highpass (fs, f0, 1.0) : BiquadCoeffs::lowpass (fs, f0, 1.0);
    }
    else
    {
        numSections = 1;
        sec[0].c = hp ? BiquadCoeffs::highpass (fs, f0, 0.70710678) : BiquadCoeffs::lowpass (fs, f0, 0.70710678);
    }
}

double ButterworthCascade::magnitude (double fs, double f) const
{
    double m = 1;
    for (int i = 0; i < numSections; ++i) m *= sec[(size_t) i].c.magnitude (fs, f);
    return m;
}
} // namespace im
