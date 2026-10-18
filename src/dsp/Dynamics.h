#pragma once
// Dynamics processors.
// Compressor: Giannoulis, Massberg, Reiss, "Digital Dynamic Range Compressor
// Design: A Tutorial and Analysis", JAES 2012. Feed-forward, log-domain,
// smooth decoupled peak detector on the gain-reduction signal, stereo-linked
// (detector input = max(|L|,|R|)).
#include <cmath>
#include <cstdlib>

namespace im
{
struct CompressorParams
{
    float thresholdDb = -20, ratio = 4, attackMs = 10, releaseMs = 100, kneeDb = 6, makeupDb = 0;
    bool autoMakeup = false;
};

// Peak clipper: tanh soft clip whose knee sits amountDb under the signal's recent peak level (instant-attack peak
// hold, ~1.5 s release), so it rounds off the top amountDb of the peaks at any level and is unity below the knee.
// First-order antiderivative anti-aliasing keeps harmonics above Nyquist from folding back. Trivially copyable.
struct PeakClipper
{
    float ratio = 1.0f, hold = 0.0f, rel = 0.0f;
    bool on = false;
    float prev[2] { 0.0f, 0.0f };

    // IM_PAPER_DYNAMICS=1: the paper's drive, a fixed knee at -driveDb dBFS (tanh(k x) / k, k = 10^(driveDb/20))
    bool fixedKnee = paperKnee();

    void prepare (double fs) noexcept { rel = (float) std::exp (-1.0 / (1.5 * fs)); reset(); }
    void reset() noexcept { hold = 0.0f; prev[0] = prev[1] = 0.0f; }
    void setAmountDb (float db) noexcept
    {
        on = db > 0.05f;
        ratio = std::pow (10.0f, -std::fmax (0.0f, db) / 20.0f);
    }
    inline void process (float& l, float& r) noexcept
    {
        if (! on) return;
        const float pk = std::fmax (std::fabs (l), std::fabs (r));
        hold = pk > hold ? pk : hold * rel;
        const float t = fixedKnee ? ratio : std::fmax (hold * ratio, 1.0e-5f); // knee, absolute
        l = shape (0, l, t);
        r = shape (1, r, t);
    }

private:
    static bool paperKnee() noexcept
    {
        const char* e = std::getenv ("IM_PAPER_DYNAMICS");
        return e != nullptr && e[0] == '1' && e[1] == 0;
    }
    static float logCosh (float z) noexcept
    {
        const float a = std::fabs (z);
        return a + std::log1p (std::exp (-2.0f * a)) - 0.69314718f;
    }
    // y = t tanh(x / t), via the antiderivative t^2 log cosh(x / t)
    inline float shape (int ch, float x, float t) noexcept
    {
        const float u = x / t, u0 = prev[ch] / t, du = u - u0;
        prev[ch] = x;
        return std::fabs (du) > 1.0e-4f ? t * (logCosh (u) - logCosh (u0)) / du : t * std::tanh (0.5f * (u + u0));
    }
};

class Compressor
{
public:
    // Static gain computer, eq. (4) of Giannoulis et al.: output level y_G for input level x_G (dB).
    static float gainComputer (float xG, float T, float R, float W) noexcept
    {
        const float d = xG - T;
        if (2.0f * d < -W) return xG;
        if (2.0f * std::fabs (d) <= W && W > 0.0f)
        {
            const float t = d + W * 0.5f;
            return xG + (1.0f / R - 1.0f) * t * t / (2.0f * W);
        }
        return T + d / R;
    }
    // Auto make-up gain: the paper's M = -(y_G - x_G) evaluated at x_G = 0 dBFS, limited to the makeup range.
    static float autoMakeupDb (float T, float R, float W) noexcept
    {
        const float m = -(gainComputer (0.0f, T, R, W) - 0.0f);
        return std::fmin (std::fmax (m, 0.0f), 24.0f);
    }

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        reset();
    }
    void reset() { y1 = yL = 0.0f; grDb = 0.0f; }
    void setParams (const CompressorParams& p) noexcept
    {
        params = p;
        alphaA = std::exp (-1.0f / (0.001f * std::fmax (p.attackMs, 0.01f) * (float) fs));
        alphaR = std::exp (-1.0f / (0.001f * std::fmax (p.releaseMs, 0.01f) * (float) fs));
        makeup = p.autoMakeup ? autoMakeupDb (p.thresholdDb, p.ratio, p.kneeDb) : p.makeupDb;
    }
    // Returns the linear gain (including makeup) for one detector sample.
    inline float processSample (float detector) noexcept
    {
        const float xG = 20.0f * std::log10 (std::fmax (std::fabs (detector), 1.0e-6f));
        const float yG = gainComputer (xG, params.thresholdDb, params.ratio, params.kneeDb);
        const float xL = xG - yG;
        // Smooth decoupled peak detector (eq. 17)
        y1 = std::fmax (xL, alphaR * y1 + (1.0f - alphaR) * xL);
        yL = alphaA * yL + (1.0f - alphaA) * y1;
        grDb = yL;
        return std::pow (10.0f, (makeup - yL) * 0.05f);
    }
    float currentGainReductionDb() const noexcept { return grDb; }

private:
    double fs = 48000.0;
    CompressorParams params;
    float alphaA = 0, alphaR = 0, makeup = 0;
    float y1 = 0, yL = 0, grDb = 0;
};

struct GateParams
{
    float thresholdDb = -50, rangeDb = 40, attackMs = 1, holdMs = 50, releaseMs = 100;
};

// Gate/expander with range (attenuation when closed), hold, attack/release
// gain ballistics. Stereo-linked detector (max |L|,|R|) with fast peak follower.
class Gate
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        envCoef = std::exp (-1.0f / (0.0005f * (float) fs)); // 0.5 ms detector release
        reset();
    }
    void reset() { env = 0; gainDb = 0; holdCounter = 0; }
    void setParams (const GateParams& p) noexcept
    {
        params = p;
        attCoef = std::exp (-1.0f / (0.001f * std::fmax (p.attackMs, 0.01f) * (float) fs));
        relCoef = std::exp (-1.0f / (0.001f * std::fmax (p.releaseMs, 0.01f) * (float) fs));
        holdSamples = (int) (p.holdMs * 0.001f * (float) fs);
        threshLin = std::pow (10.0f, p.thresholdDb * 0.05f);
    }
    inline float processSample (float detector) noexcept
    {
        const float a = std::fabs (detector);
        env = a > env ? a : envCoef * env + (1.0f - envCoef) * a;
        float target;
        if (env >= threshLin)
        {
            holdCounter = holdSamples;
            target = 0.0f;
        }
        else if (holdCounter > 0)
        {
            --holdCounter;
            target = 0.0f;
        }
        else
            target = -params.rangeDb;
        const float c = target > gainDb ? attCoef : relCoef; // opening = attack, closing = release
        gainDb = c * gainDb + (1.0f - c) * target;
        return std::pow (10.0f, gainDb * 0.05f);
    }
    float currentGainReductionDb() const noexcept { return -gainDb; }

private:
    double fs = 48000.0;
    GateParams params;
    float envCoef = 0, attCoef = 0, relCoef = 0, threshLin = 0;
    int holdSamples = 0, holdCounter = 0;
    float env = 0, gainDb = 0;
};
} // namespace im
