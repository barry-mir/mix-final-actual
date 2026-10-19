#pragma once
// ITU-R BS.1770-4 loudness: K-weighting (pre-filter shelf + RLB high-pass,
// coefficients derived for any sample rate as in libebur128), 400 ms blocks
// with 75% overlap, absolute gate -70 LUFS, relative gate -10 LU.
#include <vector>

namespace im
{
double integratedLoudness (const float* L, const float* R, int n, double fs);

// Momentary loudness (BS.1770, 400 ms window, no gating) of a running stereo signal, updated every 100 ms.
// Audio thread: prepare() allocates nothing after the first call; process() is real-time safe.
class MomentaryLoudness
{
public:
    void prepare (double fs);
    void process (const float* L, const float* R, int n) noexcept;
    float lufs() const noexcept { return value; } // -120 when silent

private:
    struct Bq
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    Bq sL, hL, sR, hR;
    double acc = 0, sub[4] {};
    int fill = 0, subLen = 4800, subPos = 0;
    float value = -120.0f;
};
} // namespace im
