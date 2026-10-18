#pragma once
// ITU-R BS.1770-4 loudness: K-weighting (pre-filter shelf + RLB high-pass,
// coefficients derived for any sample rate as in libebur128), 400 ms blocks
// with 75% overlap, absolute gate -70 LUFS, relative gate -10 LU.
#include <vector>

namespace im
{
double integratedLoudness (const float* L, const float* R, int n, double fs);
} // namespace im
