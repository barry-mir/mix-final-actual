#include "analysis/Loudness.h"
#include <algorithm>
#include <cmath>

namespace im
{
namespace
{
struct Bq
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

template <typename B>
void kWeightingFilters (double fs, B& shelf, B& hp)
{
    constexpr double kPi = 3.14159265358979323846;
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan (kPi * f0 / fs);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
        shelf.b1 = 2.0 * (K * K - Vh) / a0;
        shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf.a1 = 2.0 * (K * K - 1.0) / a0;
        shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan (kPi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        hp.b0 = 1.0;
        hp.b1 = -2.0;
        hp.b2 = 1.0;
        hp.a1 = 2.0 * (K * K - 1.0) / a0;
        hp.a2 = (1.0 - K / Q + K * K) / a0;
    }
}
} // namespace

double integratedLoudness (const float* L, const float* R, int n, double fs)
{
    const int block = (int) std::lround (0.4 * fs);
    const int hop = (int) std::lround (0.1 * fs);
    if (n < block) return -120.0;

    Bq sL {}, hL {}, sR {}, hR {};
    kWeightingFilters (fs, sL, hL);
    sR = sL;
    hR = hL;
    std::vector<double> sq ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double l = hL.process (sL.process (L[i]));
        const double r = hR.process (sR.process (R[i]));
        sq[(size_t) i] = l * l + r * r; // channel weights G_L = G_R = 1
    }
    // prefix sums for block energies
    std::vector<double> cs ((size_t) n + 1, 0.0);
    for (int i = 0; i < n; ++i) cs[(size_t) i + 1] = cs[(size_t) i] + sq[(size_t) i];

    std::vector<double> z;
    for (int s = 0; s + block <= n; s += hop)
        z.push_back ((cs[(size_t) (s + block)] - cs[(size_t) s]) / block);

    auto lufs = [] (double e) { return -0.691 + 10.0 * std::log10 (std::max (e, 1e-20)); };
    double sum = 0;
    int cnt = 0;
    for (double e : z)
        if (lufs (e) > -70.0) { sum += e; ++cnt; }
    if (cnt == 0) return -120.0;
    const double relGate = lufs (sum / cnt) - 10.0;
    sum = 0;
    cnt = 0;
    for (double e : z)
        if (lufs (e) > -70.0 && lufs (e) > relGate) { sum += e; ++cnt; }
    if (cnt == 0) return -120.0;
    return lufs (sum / cnt);
}

void MomentaryLoudness::prepare (double fs)
{
    sL = hL = sR = hR = Bq {};
    kWeightingFilters (fs, sL, hL);
    sR = sL;
    hR = hL;
    subLen = std::max (1, (int) std::lround (0.1 * fs));
    subPos = fill = 0;
    acc = 0;
    for (auto& v : sub) v = 0;
    value = -120.0f;
}

void MomentaryLoudness::process (const float* L, const float* R, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const double l = hL.process (sL.process (L[i])), r = hR.process (sR.process (R[i]));
        acc += l * l + r * r;
        if (++subPos < subLen) continue;
        sub[fill % 4] = acc / subLen;
        ++fill;
        acc = 0;
        subPos = 0;
        const double e = (sub[0] + sub[1] + sub[2] + sub[3]) / 4.0;
        value = fill >= 4 && e > 1e-12 ? (float) (-0.691 + 10.0 * std::log10 (e)) : -120.0f;
    }
}
} // namespace im
