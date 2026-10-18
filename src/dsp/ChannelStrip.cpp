#include "dsp/ChannelStrip.h"
#include <algorithm>
#include <cmath>

namespace im
{

void StripCore::prepare (double fs)
{
    gate.prepare (fs);
    comp.prepare (fs);
    dsAtt = 1.0f - (float) std::exp (-1.0 / (0.0005 * fs));
    dsRel = 1.0f - (float) std::exp (-1.0 / (0.06 * fs));
    drive.prepare (fs);
    reset();
}

void StripCore::reset()
{
    hpf.reset();
    lpf.reset();
    for (auto& b : eq) b.reset();
    gate.reset();
    comp.reset();
    dsLp.reset();
    dsEnv = 0;
    drive.reset();
    compGrMax = gateGrMax = dsGrMax = 0;
}

void StripCore::updateCoeffs (const StripBlockParams& p, double fs)
{
    if (cfg.hpfOn) hpf.design (true, fs, p.hpfFreq, cfg.hpfSlope);
    if (cfg.lpfOn) lpf.design (false, fs, p.lpfFreq, cfg.lpfSlope);
    if (cfg.eqOn)
    {
        for (int b = 0; b < 4; ++b)
        {
            if (! cfg.bandOn[(size_t) b]) continue;
            const double f = p.eqFreq[(size_t) b], g = p.eqGain[(size_t) b], q = p.eqQ[(size_t) b];
            BiquadCoeffs c;
            if (b == 0 && cfg.lfShelf) c = BiquadCoeffs::lowShelf (fs, f, g);
            else if (b == 3 && cfg.hfShelf) c = BiquadCoeffs::highShelf (fs, f, g);
            else c = BiquadCoeffs::peaking (fs, f, q, g);
            eq[(size_t) b].c = c;
        }
    }
    if (cfg.gateOn) gate.setParams (p.gate);
    if (cfg.dsOn)
    {
        dsLp.c = BiquadCoeffs::lowpass (fs, safeFreq (fs, p.dsFreq), 0.7071);
        dsThrDb = p.dsThrDb;
    }
    drive.setAmountDb (p.driveDb);
    if (cfg.compOn)
    {
        auto cp = p.comp;
        cp.autoMakeup = cfg.autoMakeup;
        comp.setParams (cp);
    }
}

void StripCore::process (const float* inL, const float* inR, int n, const StripGains& g,
                         float* outL, float* outR, float* sendL, float* sendR) noexcept
{
    const float pl = cfg.polL ? -1.0f : 1.0f, pr = cfg.polR ? -1.0f : 1.0f;
    compGrMax = gateGrMax = dsGrMax = 0;
    for (int i = 0; i < n; ++i)
    {
        float l = inL[i] * g.trim[i] * pl;
        float r = inR[i] * g.trim[i] * pr;

        if (cfg.hpfOn) { l = hpf.process (0, l); r = hpf.process (1, r); }
        if (cfg.lpfOn) { l = lpf.process (0, l); r = lpf.process (1, r); }
        if (cfg.gateOn)
        {
            const float gg = gate.processSample (std::max (std::fabs (l), std::fabs (r)));
            l *= gg;
            r *= gg;
            gateGrMax = std::max (gateGrMax, gate.currentGainReductionDb());
        }

        auto doEq = [&] {
            if (! cfg.eqOn) return;
            for (size_t b = 0; b < 4; ++b)
                if (cfg.bandOn[b]) { l = eq[b].process (0, l); r = eq[b].process (1, r); }
        };
        auto doComp = [&] {
            if (! cfg.compOn) return;
            const float cg = comp.processSample (std::max (std::fabs (l), std::fabs (r)));
            l *= cg;
            r *= cg;
            compGrMax = std::max (compGrMax, comp.currentGainReductionDb());
        };
        if (cfg.compFirst) { doComp(); doEq(); }
        else { doEq(); doComp(); }

        if (cfg.dsOn)
        {
            const float hl = l - dsLp.process (0, l), hr = r - dsLp.process (1, r); // the band above freq
            const float x = std::max (std::fabs (hl), std::fabs (hr));
            dsEnv += (x > dsEnv ? dsAtt : dsRel) * (x - dsEnv);
            const float over = 20.0f * std::log10 (std::max (dsEnv, 1.0e-9f)) - dsThrDb;
            const float gr = over > 0.0f ? std::min (12.0f, over * 0.75f) : 0.0f;
            const float keep = gr > 0.0f ? std::pow (10.0f, -0.05f * gr) : 1.0f;
            l -= (1.0f - keep) * hl;
            r -= (1.0f - keep) * hr;
            dsGrMax = std::max (dsGrMax, gr);
        }
        drive.process (l, r);

        // Width (M/S)
        const float m = 0.5f * (l + r), s = 0.5f * (l - r) * g.width[i];
        l = m + s;
        r = m - s;
        // Balance (constant power)
        l *= g.balL[i];
        r *= g.balR[i];

        const float mute = g.mute[i];
        if (cfg.sendPre)
        {
            sendL[i] = l * g.send[i] * mute;
            sendR[i] = r * g.send[i] * mute;
        }
        l *= g.fader[i];
        r *= g.fader[i];
        if (! cfg.sendPre)
        {
            sendL[i] = l * g.send[i] * mute;
            sendR[i] = r * g.send[i] * mute;
        }
        outL[i] = l * mute;
        outR[i] = r * mute;
    }
}

void ChannelStrip::prepare (double sampleRate, int maxBlock)
{
    fs = sampleRate;
    doubler.prepare (fs);
    for (auto& c : cores) c.prepare (fs);
    for (auto& core : scratch)
        for (auto& v : core) v.assign ((size_t) maxBlock, 0.0f);
    reset();
}

void ChannelStrip::reset()
{
    doubler.reset();
    for (auto& c : cores) c.reset();
    active = { true, false, false };
    for (int k = 1; k < kCores; ++k) active[(size_t) k] = false;
    active[(size_t) newest] = true;
    xfading = false;
    hasPending = false;
}

int ChannelStrip::activeCoreCount() const noexcept
{
    return (int) std::count (active.begin(), active.end(), true);
}

void ChannelStrip::setConfig (const StripConfig& c, int rampSamples) noexcept
{
    if (c == targetConfig()) return;
    if (! xfading && ! hasPending && rampSamples <= 0)
    {
        cores[(size_t) newest].cfg = c;
        return;
    }
    startTransition (c, std::max (1, rampSamples));
}

void ChannelStrip::startTransition (const StripConfig& c, int rampSamples) noexcept
{
    int freeSlot = -1;
    for (int k = 0; k < kCores; ++k)
        if (! active[(size_t) k]) { freeSlot = k; break; }
    if (freeSlot < 0)
    {
        // All paths busy: start as soon as one frees up (end of the current fade).
        hasPending = true;
        pendingCfg = c;
        pendingRamp = rampSamples;
        return;
    }
    hasPending = false;

    // Current powers become the starting powers of the fade-outs.
    const float t = xfading ? std::min (1.0f, (float) xfadePos / (float) xfadeLen) : 1.0f;
    for (int k = 0; k < kCores; ++k)
        power0[(size_t) k] = active[(size_t) k] ? corePower (k, t) : 0.0f;

    auto& src = cores[(size_t) newest];
    auto& dst = cores[(size_t) freeSlot];
    dst = src; // warm state copy (trivially copyable, no allocation)
    const auto old = src.cfg;
    dst.cfg = c;
    if (! old.hpfOn || old.hpfSlope != c.hpfSlope) dst.hpf.reset();
    if (! old.lpfOn || old.lpfSlope != c.lpfSlope) dst.lpf.reset();
    if (! old.gateOn) dst.gate.reset();
    if (! old.compOn) dst.comp.reset();
    if (! old.dsOn) { dst.dsLp.reset(); dst.dsEnv = 0; }
    for (size_t b = 0; b < 4; ++b)
        if (! old.eqOn || ! old.bandOn[b] || (b == 0 && old.lfShelf != c.lfShelf) || (b == 3 && old.hfShelf != c.hfShelf))
            dst.eq[b].reset();

    newest = freeSlot;
    active[(size_t) freeSlot] = true;
    power0[(size_t) freeSlot] = 0.0f;
    xfading = true;
    xfadePos = 0;
    xfadeLen = rampSamples;
}

void ChannelStrip::process (const float* inL, const float* inR, int n, const StripBlockParams& p, const StripGains& g,
                            float* outL, float* outR, float* sendL, float* sendR) noexcept
{
    for (int k = 0; k < kCores; ++k)
        if (active[(size_t) k]) cores[(size_t) k].updateCoeffs (p, fs);

    if (! xfading)
    {
        cores[(size_t) newest].process (inL, inR, n, g, outL, outR, sendL, sendR);
        if (doubler.isActive()) doubler.process (outL, outR, n);
        if (hasPending) startTransition (pendingCfg, pendingRamp);
        return;
    }

    std::fill (outL, outL + n, 0.0f);
    std::fill (outR, outR + n, 0.0f);
    std::fill (sendL, sendL + n, 0.0f);
    std::fill (sendR, sendR + n, 0.0f);
    for (int k = 0; k < kCores; ++k)
    {
        if (! active[(size_t) k]) continue;
        auto& sc = scratch[(size_t) k];
        cores[(size_t) k].process (inL, inR, n, g, sc[0].data(), sc[1].data(), sc[2].data(), sc[3].data());
        for (int i = 0; i < n; ++i)
        {
            const float t = std::min (1.0f, (float) (xfadePos + i) / (float) xfadeLen);
            const float w = std::sqrt (std::max (0.0f, corePower (k, t))); // equal-power
            outL[i] += w * sc[0][(size_t) i];
            outR[i] += w * sc[1][(size_t) i];
            sendL[i] += w * sc[2][(size_t) i];
            sendR[i] += w * sc[3][(size_t) i];
        }
    }
    if (doubler.isActive()) doubler.process (outL, outR, n);
    xfadePos += n;
    if (xfadePos >= xfadeLen)
    {
        xfading = false;
        for (int k = 0; k < kCores; ++k)
            active[(size_t) k] = (k == newest);
        if (hasPending) startTransition (pendingCfg, pendingRamp);
    }
}
} // namespace im
