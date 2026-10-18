#include "dsp/Limiter.h"
#include <algorithm>
#include <cmath>

namespace im
{
namespace
{
int nextPow2 (int v)
{
    int c = 1;
    while (c < v) c <<= 1;
    return c;
}
} // namespace

void Limiter::prepare (double sampleRate, int)
{
    fs = sampleRate;
    latency = std::max (interpDelay + 1, (int) std::lround (0.0015 * fs));
    window = latency - interpDelay + 1;

    // Windowed-sinc 4x interpolator: phase p estimates x(n - D + p/4) from x[n-j], j = 0..7.
    constexpr double kPi = 3.14159265358979323846;
    for (int p = 0; p < kOversample; ++p)
    {
        double sum = 0;
        for (int j = 0; j < kTapsPerPhase; ++j)
        {
            const double x = (double) j - interpDelay + (double) p / kOversample;
            const double sinc = std::abs (x) < 1e-9 ? 1.0 : std::sin (kPi * x) / (kPi * x);
            const double w = 0.5 + 0.5 * std::cos (kPi * x / (kTapsPerPhase * 0.5 + 0.5));
            poly[(size_t) p][(size_t) j] = (float) (sinc * w);
            sum += sinc * w;
        }
        for (auto& c : poly[(size_t) p]) c = (float) (c / sum);
    }

    const int dSize = nextPow2 (latency + 2);
    dL.assign ((size_t) dSize, 0.0f);
    dR.assign ((size_t) dSize, 0.0f);
    dMask = dSize - 1;
    const int qSize = nextPow2 (window + 2);
    minVal.assign ((size_t) qSize, 1.0f);
    minIdx.assign ((size_t) qSize, 0);
    qMask = qSize - 1;
    box.assign ((size_t) window, 1.0f);
    releaseCoef = std::exp (-1.0f / (0.080f * (float) fs)); // 80 ms release
    reset();
}

void Limiter::reset()
{
    for (auto& h : hist) h.fill (0.0f);
    histPos = 0;
    std::fill (dL.begin(), dL.end(), 0.0f);
    std::fill (dR.begin(), dR.end(), 0.0f);
    dW = 0;
    qHead = qTail = 0;
    std::fill (box.begin(), box.end(), 1.0f);
    boxPos = 0;
    boxSum = (double) window;
    sampleIdx = 0;
    env = 1.0f;
    grDb = 0.0f;
}

void Limiter::process (float* l, float* r, int n, float ceilingDb, const float* onWeight) noexcept
{
    const float ceiling = std::pow (10.0f, std::clamp (ceilingDb, -1.0f, 0.0f) * 0.05f) * 0.999f;
    float maxGr = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float xl = l[i], xr = r[i];
        // history (newest at histPos)
        histPos = (histPos + kTapsPerPhase - 1) % kTapsPerPhase;
        hist[0][(size_t) histPos] = xl;
        hist[1][(size_t) histPos] = xr;

        float tp = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto& h = hist[(size_t) ch];
            for (int p = 0; p < kOversample; ++p)
            {
                float acc = 0.0f;
                const auto& c = poly[(size_t) p];
                for (int j = 0; j < kTapsPerPhase; ++j)
                    acc += c[(size_t) j] * h[(size_t) ((histPos + j) % kTapsPerPhase)];
                tp = std::max (tp, std::fabs (acc));
            }
        }
        const float req = tp > ceiling ? ceiling / tp : 1.0f;

        // sliding minimum over `window`
        while (qTail != qHead && minVal[(size_t) ((qTail - 1) & qMask)] >= req) qTail = (qTail - 1) & qMask;
        minVal[(size_t) qTail] = req;
        minIdx[(size_t) qTail] = sampleIdx;
        qTail = (qTail + 1) & qMask;
        while (minIdx[(size_t) qHead] <= sampleIdx - window) qHead = (qHead + 1) & qMask;
        const float held = minVal[(size_t) qHead];

        env = held < env ? held : held + releaseCoef * (env - held);

        boxSum += (double) env - (double) box[(size_t) boxPos];
        box[(size_t) boxPos] = env;
        boxPos = boxPos + 1 == window ? 0 : boxPos + 1;
        if ((sampleIdx & 0xFFFF) == 0)
        {
            boxSum = 0;
            for (auto v : box) boxSum += v;
        }
        const float g = (float) (boxSum / window);

        // audio delay of `latency`
        dL[(size_t) dW] = xl;
        dR[(size_t) dW] = xr;
        const int rd = (dW - latency) & dMask;
        dW = (dW + 1) & dMask;
        const float w = onWeight ? onWeight[i] : 1.0f;
        const float gain = 1.0f - w * (1.0f - std::min (g, 1.0f));
        l[i] = dL[(size_t) rd] * gain;
        r[i] = dR[(size_t) rd] * gain;
        maxGr = std::max (maxGr, -20.0f * std::log10 (std::max (gain, 1e-6f)));
        ++sampleIdx;
    }
    grDb = maxGr;
}
} // namespace im
