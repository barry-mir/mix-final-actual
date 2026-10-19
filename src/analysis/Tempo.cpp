#include "analysis/Tempo.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace im
{
namespace
{
constexpr int kFftOrder = 10, kFrame = 1 << kFftOrder, kHop = kFrame / 2;

// Onset strength per hop: half-wave rectified rise of the log-compressed magnitude spectrum, minus its local mean.
std::vector<double> onsetStrength (const float* x, int n)
{
    std::vector<double> out;
    if (n < kFrame * 4) return out;
    juce::dsp::FFT fft (kFftOrder);
    std::vector<float> win ((size_t) kFrame), buf ((size_t) kFrame * 2), prev ((size_t) kFrame / 2 + 1, 0.0f);
    for (int i = 0; i < kFrame; ++i) win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (6.283185307179586 * i / (kFrame - 1)));
    for (int s = 0; s + kFrame <= n; s += kHop)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < kFrame; ++i) buf[(size_t) i] = x[s + i] * win[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        double flux = 0;
        for (int k = 1; k <= kFrame / 2; ++k)
        {
            const float m = std::log1p (1000.0f * buf[(size_t) k]);
            flux += std::max (0.0f, m - prev[(size_t) k]);
            prev[(size_t) k] = m;
        }
        out.push_back (flux);
    }
    if (! out.empty()) out[0] = 0; // the first frame rises from nothing
    // subtract a ~0.5 s moving average, keep the rises
    const int w = 24;
    std::vector<double> cs (out.size() + 1, 0.0);
    for (size_t i = 0; i < out.size(); ++i) cs[i + 1] = cs[i] + out[i];
    std::vector<double> r (out.size());
    for (size_t i = 0; i < out.size(); ++i)
    {
        const size_t a = i >= (size_t) w ? i - (size_t) w : 0, b = std::min (out.size(), i + (size_t) w + 1);
        r[i] = std::max (0.0, out[i] - (cs[b] - cs[a]) / (double) (b - a));
    }
    return r;
}

double at (const std::vector<double>& v, double i) // linear interpolation, 0 outside
{
    if (i < 0 || i > (double) v.size() - 1) return 0.0;
    const auto k = (size_t) i;
    const double f = i - (double) k;
    return k + 1 < v.size() ? v[k] * (1 - f) + v[k + 1] * f : v[k];
}

// Mean onset strength on the comb phase, phase + period, phase + 2 period, ...
double comb (const std::vector<double>& o, double period, double phase)
{
    double s = 0;
    int c = 0;
    for (double t = phase; t < (double) o.size() - 1; t += period, ++c) s += std::max ({ at (o, t - 1), at (o, t), at (o, t + 1) });
    return c ? s / c : 0.0;
}
} // namespace

TempoEstimate estimateTempo (const float* x, int n, double fs, int beatsPerBar)
{
    TempoEstimate e;
    const auto o = onsetStrength (x, n);
    if (o.size() < 200 || fs <= 0) return e;
    const double hopSec = kHop / fs;
    double energy = 0;
    for (double v : o) energy += v * v;
    if (energy <= 1e-12) return e;

    // 1. coarse: autocorrelation at the lag of each tempo (with its double), weighted toward 90-160 BPM
    auto acf = [&] (double lag) {
        double s = 0;
        for (size_t i = 0; i + (size_t) std::ceil (lag) + 1 < o.size(); ++i) s += o[i] * at (o, (double) i + lag);
        return s / energy;
    };
    // 1b. the strongest periodicity (no preference yet)
    auto score = [&] (double bpm) {
        const double lag = 60.0 / bpm / hopSec;
        return acf (lag) + 0.5 * acf (2 * lag);
    };
    double best = -1e9, bestBpm = 120;
    for (double bpm = 60; bpm <= 200; bpm += 0.5)
        if (const double sc = score (bpm); sc > best)
        {
            best = sc;
            bestBpm = bpm;
        }
    // 2. fine: around a tempo (+-4%, 0.02 BPM steps), the period and phase whose beats land on the strongest onsets
    struct Fine
    {
        double bpm = 0, period = 0, phase = 0, score = -1;
    };
    auto fine = [&] (double centre) {
        Fine f;
        for (double bpm = centre * 0.96; bpm <= centre * 1.04; bpm += 0.02)
        {
            const double period = 60.0 / bpm / hopSec;
            for (double ph = 0; ph < period; ph += 0.25)
                if (const double sc = comb (o, period, ph); sc > f.score)
                    f = { bpm, period, ph, sc };
        }
        return f;
    };
    // The octaves of the strongest periodicity compete on the comb score; one inside 90-160 BPM wins when it is nearly
    // as good (a period that is not an octave, e.g. 4/3 of the beat in a shuffle, never competes).
    std::vector<Fine> cands { fine (bestBpm) };
    for (double c : { bestBpm * 2.0, bestBpm / 2.0 })
        if (c >= 50.0 && c <= 220.0) cands.push_back (fine (c));
    Fine pick = cands[0];
    for (auto& c : cands)
        if (c.score > pick.score) pick = c;
    for (auto& c : cands)
        if (c.bpm >= 90.0 && c.bpm <= 160.0 && ! (pick.bpm >= 90.0 && pick.bpm <= 160.0) && c.score >= 0.8 * pick.score) pick = c;
    const double bestPeriod = pick.period, bestPhase = pick.phase, bestScore = pick.score;
    e.bpm = std::round (60.0 / (bestPeriod * hopSec) * 100.0) / 100.0;
    // 3. bar 1: of the beats in one bar, the one with the most energy just after it (the onset curve is log
    // compressed, so the raw signal tells the loud downbeat apart better).
    // A flux frame starting at hop i first holds an attack about kFrame - kHop/2 samples later.
    const double firstBeat = bestPhase * hopSec + (kFrame - kHop / 2) / fs, beatLen = bestPeriod * hopSec;
    const int win = std::max (1, (int) (0.05 * fs));
    int down = 0;
    double downScore = -1;
    for (int b = 0; b < beatsPerBar; ++b)
    {
        double sum = 0;
        int cnt = 0;
        for (double t = firstBeat + b * beatLen - 0.01; t * fs + win < n; t += beatLen * beatsPerBar, ++cnt)
        {
            if (t < 0) continue;
            const int s0 = (int) (t * fs);
            for (int i = s0; i < s0 + win; ++i) sum += (double) x[i] * x[i];
        }
        const double v = cnt ? sum / cnt : 0.0;
        if (v > downScore)
        {
            downScore = v;
            down = b;
        }
    }
    const double beatSec = firstBeat + down * beatLen;
    const double barSec = 60.0 / e.bpm * beatsPerBar;
    e.offsetSec = std::fmod (std::fmod (beatSec, barSec) + barSec, barSec);
    double mean = 0;
    for (double v : o) mean += v;
    mean /= (double) o.size();
    e.confidence = std::clamp (mean > 0 ? (bestScore / mean - 1.0) / 4.0 : 0.0, 0.0, 1.0);
    e.ok = true;
    return e;
}
} // namespace im
