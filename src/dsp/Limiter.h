#pragma once
// True-peak-safe lookahead limiter (1.5 ms lookahead).
// Detection runs on 4x oversampled (windowed-sinc polyphase) estimates of the
// inter-sample peak (as in ITU-R BS.1770 true-peak metering). The required
// gain is min-held over W samples and smoothed with a W-sample boxcar, with the
// audio delayed so the smoothed gain reaches its minimum exactly at the peak:
//   audio delay L = D_interp + W - 1.
// Release is a one-pole on the held gain. Latency L is constant (also when
// bypassed) and is reported to the host.
#include <array>
#include <vector>

namespace im
{
class Limiter
{
public:
    static constexpr int kOversample = 4;
    static constexpr int kTapsPerPhase = 8;

    void prepare (double sampleRate, int maxBlock);
    void reset();
    int latencySamples() const noexcept { return latency; }

    // ceilingDb in [-1, 0]. onWeight[i] in [0,1]: 0 = bypass (delayed dry), 1 = limiting.
    void process (float* l, float* r, int n, float ceilingDb, const float* onWeight) noexcept;
    float currentGainReductionDb() const noexcept { return grDb; }

private:
    double fs = 48000.0;
    int latency = 0, window = 1, interpDelay = kTapsPerPhase / 2;
    float releaseCoef = 0.999f;
    std::array<std::array<float, kTapsPerPhase>, kOversample> poly {};
    // input history for interpolation
    std::array<std::array<float, kTapsPerPhase>, 2> hist {};
    int histPos = 0;
    // audio delay line
    std::vector<float> dL, dR;
    int dMask = 0, dW = 0;
    // sliding minimum (monotonic deque in ring) and boxcar
    std::vector<float> minVal;
    std::vector<int64_t> minIdx;
    int qHead = 0, qTail = 0, qMask = 0;
    std::vector<float> box;
    int boxPos = 0;
    double boxSum = 0.0;
    int64_t sampleIdx = 0;
    float env = 1.0f, grDb = 0.0f;
};
} // namespace im
