#include "dsp/MixEngine.h"
#include <algorithm>
#include <cmath>

namespace im
{
namespace
{
inline float balanceL (float b) { return std::cos ((b + 1.0f) * 0.78539816f); } // -3 dB at centre
inline float balanceR (float b) { return std::sin ((b + 1.0f) * 0.78539816f); }

void resetSmoother (juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>& s, float v)
{
    s.reset (1);
    s.setCurrentAndTargetValue (v);
}

void rampSmoother (juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>& s, float target, int samples)
{
    if (target == s.getTargetValue()) return;
    const float now = s.getCurrentValue();
    s.setCurrentAndTargetValue (now);
    s.reset (std::max (1, samples));
    s.setTargetValue (target);
}
} // namespace

void MixEngine::prepare (double sampleRate, int maxBlock)
{
    fs = sampleRate;
    maxBlockSize = std::max (1, maxBlock);
    params.prepare (fs);
    const size_t mb = (size_t) maxBlockSize;
    for (auto* v : { &gTrim, &gWidth, &gBalL, &gBalR, &gFader, &gSend, &gMute, &tOutL, &tOutR, &tSendL, &tSendR,
                     &sumL, &sumR, &sendSumL, &sendSumR, &wetL, &wetR, &tmp, &limW, &echoSumL, &echoSumR, &gEcho })
        v->assign (mb, 0.0f);
    inter.assign (mb * 2, 0.0f);

    bool anySolo = false;
    for (int t = 0; t < maxTracks(); ++t) anySolo |= params.boolValue (trackParam (t, Solo));
    for (int t = 0; t < maxTracks(); ++t)
    {
        auto& s = strips[(size_t) t];
        s.prepare (fs, maxBlockSize);
        s.setConfig (configFromParams (params, t), 0);
        audible[(size_t) t] = ! params.boolValue (trackParam (t, Mute)) && (! anySolo || params.boolValue (trackParam (t, Solo)));
        resetSmoother (audiblePower[(size_t) t], audible[(size_t) t] ? 1.0f : 0.0f);
    }
    reverb.prepare (fs, maxBlockSize);
    echo.prepare (fs);
    for (int g = 0; g < kNumGroupBuses; ++g)
    {
        groupComp[(size_t) g].prepare (fs);
        groupClip[(size_t) g].prepare (fs);
        resetSmoother (groupOnPower[(size_t) g], params.boolValue (masterParam (groupBusSlot (g, GbOn))) ? 1.0f : 0.0f);
        groupL[(size_t) g].assign (mb, 0.0f);
        groupR[(size_t) g].assign (mb, 0.0f);
    }
    busComp.prepare (fs);
    limiter.prepare (fs, maxBlockSize);
    resetSmoother (busOnPower, params.boolValue (masterParam (BcOn)) ? 1.0f : 0.0f);
    resetSmoother (busMakeupDb, 0.0f);
    resetSmoother (limOn, params.boolValue (masterParam (LimOn)) ? 1.0f : 0.0f);

    int dsz = 1;
    while (dsz < limiter.latencySamples() + 2) dsz <<= 1;
    dryL.assign ((size_t) dsz, 0.0f);
    dryR.assign ((size_t) dsz, 0.0f);
    dryMask = dsz - 1;
    dryPos = 0;
    bypassW.reset (fs, 0.02);
    bypassW.setCurrentAndTargetValue (bypass.load() ? 1.0f : 0.0f);
    powCoef = (float) std::exp (-1.0 / (3.0 * fs));   // 3 s RMS integration
    matchCoef = (float) std::exp (-1.0 / (0.5 * fs)); // 0.5 s gain glide
    procPow = dryPow = 1e-6;
    matchGain = 1.0f;
}

StripConfig MixEngine::configFromParams (const ParamEngine& pe, int t) noexcept
{
    auto b = [&] (int slot) { return pe.boolValue (trackParam (t, slot)); };
    auto i = [&] (int slot) { return pe.intValue (trackParam (t, slot)); };
    StripConfig c;
    c.polL = b (PolL);
    c.polR = b (PolR);
    c.hpfOn = b (HpfOn);
    c.hpfSlope = 12 + 6 * i (HpfSlope);
    c.lpfOn = b (LpfOn);
    c.lpfSlope = i (LpfSlope) == 0 ? 12 : 24;
    c.gateOn = b (GateOn);
    c.eqOn = b (EqOn);
    c.bandOn = { b (LfOn), b (LmfOn), b (HmfOn), b (HfOn) };
    c.lfShelf = i (LfShape) == 0;
    c.hfShelf = i (HfShape) == 0;
    c.compOn = b (CompOn);
    c.autoMakeup = b (CompAutoMakeup);
    c.compFirst = i (Order) == 1;
    c.dsOn = b (DsOn);
    c.sendPre = b (SendPre);
    return c;
}

StripBlockParams MixEngine::blockParamsFromParams (const ParamEngine& pe, int t) noexcept
{
    auto v = [&] (int slot) { return pe.value (trackParam (t, slot)); };
    StripBlockParams p;
    p.hpfFreq = v (HpfFreq);
    p.lpfFreq = v (LpfFreq);
    p.gate = { v (GateThr), v (GateRange), v (GateAtt), v (GateHold), v (GateRel) };
    p.eqFreq = { v (LfFreq), v (LmfFreq), v (HmfFreq), v (HfFreq) };
    p.eqGain = { v (LfGain), v (LmfGain), v (HmfGain), v (HfGain) };
    p.eqQ = { v (LfQ), v (LmfQ), v (HmfQ), v (HfQ) };
    p.comp.thresholdDb = v (CompThr);
    p.comp.ratio = v (CompRatio);
    p.comp.attackMs = v (CompAtt);
    p.comp.releaseMs = v (CompRel);
    p.comp.kneeDb = v (CompKnee);
    p.comp.makeupDb = v (CompMakeup);
    p.dsThrDb = v (DsThr);
    p.dsFreq = v (DsFreq);
    p.driveDb = v (Drive);
    return p;
}

void MixEngine::fillRamp (float* dst, float a, float b, int n) noexcept
{
    if (a == b)
    {
        std::fill (dst, dst + n, a);
        return;
    }
    const float step = (b - a) / (float) n;
    for (int i = 0; i < n; ++i) dst[i] = a + step * (float) (i + 1);
}

void MixEngine::pushInterleaved (SpscRing<float>& ring, const float* l, const float* r, int n) noexcept
{
    float* d = inter.data();
    for (int i = 0; i < n; ++i)
    {
        d[2 * i] = l[i];
        d[2 * i + 1] = r[i];
    }
    const size_t w = ring.pushSome (d, (size_t) n * 2);
    if (w < (size_t) n * 2 && taps) taps->droppedSamples.fetch_add ((size_t) n * 2 - w, std::memory_order_relaxed);
}

void MixEngine::process (const float* const* inL, const float* const* inR, float* outL, float* outR, int n) noexcept
{
    jassert (n <= maxBlockSize);
    params.beginBlock (n);

    std::fill (sumL.begin(), sumL.begin() + n, 0.0f);
    std::fill (sumR.begin(), sumR.begin() + n, 0.0f);
    std::fill (sendSumL.begin(), sendSumL.begin() + n, 0.0f);
    std::fill (sendSumR.begin(), sendSumR.begin() + n, 0.0f);
    std::fill (echoSumL.begin(), echoSumL.begin() + n, 0.0f);
    std::fill (echoSumR.begin(), echoSumR.begin() + n, 0.0f);
    for (int g = 0; g < kNumGroupBuses; ++g)
    {
        std::fill (groupL[(size_t) g].begin(), groupL[(size_t) g].begin() + n, 0.0f);
        std::fill (groupR[(size_t) g].begin(), groupR[(size_t) g].begin() + n, 0.0f);
    }

    // Solo-in-place / mute -> per-track audibility with crossfade ramps.
    bool anySolo = false;
    int soloRamp = -1;
    for (int t = 0; t < maxTracks(); ++t)
    {
        anySolo |= params.boolValue (trackParam (t, Solo));
        soloRamp = std::max (soloRamp, params.discreteChangeRamp (trackParam (t, Solo)));
    }

    for (int t = 0; t < maxTracks(); ++t)
    {
        const size_t ts = (size_t) t;
        auto& m = meters[ts];
        const bool aud = ! params.boolValue (trackParam (t, Mute)) && (! anySolo || params.boolValue (trackParam (t, Solo)));
        if (aud != audible[ts])
        {
            audible[ts] = aud;
            int ramp = std::max (soloRamp, params.discreteChangeRamp (trackParam (t, Mute)));
            if (ramp <= 0) ramp = (int) (ParamEngine::kManualRampSec * fs);
            rampSmoother (audiblePower[ts], aud ? 1.0f : 0.0f, ramp);
        }

        if (inL[ts] == nullptr || inR[ts] == nullptr)
        {
            m.inPeak = m.inRms = m.outPeak = m.outRms = m.compGr = m.gateGr = 0.0f;
            audiblePower[ts].skip (n);
            continue;
        }

        // Discrete configuration (crossfaded on change).
        auto& strip = strips[ts];
        const auto cfg = configFromParams (params, t);
        if (! (cfg == strip.targetConfig()))
        {
            int ramp = -1;
            for (int slot : { PolL, PolR, HpfOn, HpfSlope, LpfOn, LpfSlope, GateOn, EqOn, LfOn, LfShape, LmfOn, HmfOn, HfOn,
                              HfShape, CompOn, CompAutoMakeup, Order, SendPre })
                ramp = std::max (ramp, params.discreteChangeRamp (trackParam (t, slot)));
            if (ramp <= 0) ramp = (int) (ParamEngine::kManualRampSec * fs);
            strip.setConfig (cfg, ramp);
        }

        // Per-sample gain curves (linear interpolation between block ends).
        auto pv = [&] (int slot) { return params.prevValue (trackParam (t, slot)); };
        auto cv = [&] (int slot) { return params.value (trackParam (t, slot)); };
        fillRamp (gTrim.data(), dbToGain (pv (TrimGain)), dbToGain (cv (TrimGain)), n);
        fillRamp (gWidth.data(), pv (Width), cv (Width), n);
        fillRamp (gBalL.data(), balanceL (pv (Balance)), balanceL (cv (Balance)), n);
        fillRamp (gBalR.data(), balanceR (pv (Balance)), balanceR (cv (Balance)), n);
        fillRamp (gFader.data(), dbToGain (pv (Fader)), dbToGain (cv (Fader)), n);
        fillRamp (gSend.data(), dbToGain (pv (SendGain)), dbToGain (cv (SendGain)), n);
        {
            auto& ap = audiblePower[ts];
            const float a = std::sqrt (std::max (0.0f, ap.getCurrentValue()));
            ap.skip (n);
            const float b = std::sqrt (std::max (0.0f, ap.getCurrentValue()));
            fillRamp (gMute.data(), a, b, n);
        }
        const StripGains g { gTrim.data(), gWidth.data(), gBalL.data(), gBalR.data(), gFader.data(), gSend.data(), gMute.data() };
        {
            auto dv = [&] (int slot) { return params.value (trackParam (t, slot)); };
            DoublerParams dp;
            dp.delayMs = dv (DblDelay);
            dp.spreadMs = dv (DblSpread);
            dp.depthMs = dv (DblDepth);
            dp.rateHz = dv (DblRate);
            dp.mix = dv (DblOn) > 0.5f ? dv (DblMix) : 0.0f; // off = dry, but the delay line keeps running
            strip.setDoubler (dp);
        }

        strip.process (inL[ts], inR[ts], n, blockParamsFromParams (params, t), g,
                       tOutL.data(), tOutR.data(), tSendL.data(), tSendR.data());

        const int grp = trackGroup[ts].load (std::memory_order_relaxed);
        float* dstL = grp >= 0 && grp < kNumGroupBuses ? groupL[(size_t) grp].data() : sumL.data();
        float* dstR = grp >= 0 && grp < kNumGroupBuses ? groupR[(size_t) grp].data() : sumR.data();
        float ip = 0, is = 0, op = 0, os = 0;
        for (int i = 0; i < n; ++i)
        {
            const float a = std::max (std::fabs (inL[ts][i]), std::fabs (inR[ts][i]));
            ip = std::max (ip, a);
            is += 0.5f * (inL[ts][i] * inL[ts][i] + inR[ts][i] * inR[ts][i]);
            const float o = std::max (std::fabs (tOutL[(size_t) i]), std::fabs (tOutR[(size_t) i]));
            op = std::max (op, o);
            os += 0.5f * (tOutL[(size_t) i] * tOutL[(size_t) i] + tOutR[(size_t) i] * tOutR[(size_t) i]);
            dstL[i] += tOutL[(size_t) i];
            dstR[i] += tOutR[(size_t) i];
            sendSumL[(size_t) i] += tSendL[(size_t) i];
            sendSumR[(size_t) i] += tSendR[(size_t) i];
        }
        // echo send: post-fader, post-pan (the track output, so mute/solo apply)
        fillRamp (gEcho.data(), dbToGain (params.prevValue (trackParam (t, EchoSend))), dbToGain (params.value (trackParam (t, EchoSend))), n);
        if (gEcho[0] > 0.0f || gEcho[(size_t) n - 1] > 0.0f)
            for (int i = 0; i < n; ++i)
            {
                echoSumL[(size_t) i] += tOutL[(size_t) i] * gEcho[(size_t) i];
                echoSumR[(size_t) i] += tOutR[(size_t) i] * gEcho[(size_t) i];
            }
        m.inPeak.store (ip, std::memory_order_relaxed);
        m.inRms.store (std::sqrt (is / (float) n), std::memory_order_relaxed);
        m.outPeak.store (op, std::memory_order_relaxed);
        m.outRms.store (std::sqrt (os / (float) n), std::memory_order_relaxed);
        m.compGr.store (strip.compGainReductionDb(), std::memory_order_relaxed);
        m.gateGr.store (strip.gateGainReductionDb(), std::memory_order_relaxed);

        if (taps)
        {
            pushInterleaved (taps->pre[ts], inL[ts], inR[ts], n);
            pushInterleaved (taps->post[ts], tOutL.data(), tOutR.data(), n);
            taps->gr[ts].push ({ strip.compGainReductionDb(), strip.gateGainReductionDb(), n });
        }
    }

    // ---- Instrument-group buses: compressor (equal-power on/off) then peak clipper, into the mix ----------
    for (int g = 0; g < kNumGroupBuses; ++g)
    {
        auto gv = [&] (int f) { return params.value (masterParam (groupBusSlot (g, f))); };
        auto& comp = groupComp[(size_t) g];
        auto& onP = groupOnPower[(size_t) g];
        const int on = masterParam (groupBusSlot (g, GbOn));
        const int r = params.discreteChangeRamp (on);
        if (r > 0) rampSmoother (onP, params.boolValue (on) ? 1.0f : 0.0f, r);
        CompressorParams cp;
        cp.thresholdDb = gv (GbThr);
        cp.ratio = gv (GbRatio);
        cp.attackMs = gv (GbAtt);
        cp.releaseMs = gv (GbRel);
        cp.makeupDb = gv (GbMakeup);
        comp.setParams (cp);
        auto& clip = groupClip[(size_t) g];
        clip.setAmountDb (gv (GbClip));
        float* bl = groupL[(size_t) g].data();
        float* br = groupR[(size_t) g].data();
        const bool idle = ! onP.isSmoothing() && onP.getCurrentValue() <= 0.0f;
        float grMax = 0;
        double inE = 0, outE = 0;
        for (int i = 0; i < n; ++i)
        {
            float l = bl[i], rr = br[i];
            inE += (double) l * l + (double) rr * rr;
            const float gain = comp.processSample (std::max (std::fabs (l), std::fabs (rr)));
            if (! idle)
            {
                const float p = std::clamp (onP.getNextValue(), 0.0f, 1.0f); // a linear ramp can step just past 0 or 1
                const float w = std::sqrt (1.0f - p) + std::sqrt (p) * gain;
                l *= w;
                rr *= w;
                grMax = std::max (grMax, comp.currentGainReductionDb());
            }
            clip.process (l, rr);
            outE += (double) l * l + (double) rr * rr;
            sumL[(size_t) i] += l;
            sumR[(size_t) i] += rr;
        }
        masterMeters.groupGr[(size_t) g].store (idle ? 0.0f : grMax, std::memory_order_relaxed);
        const double a = std::exp (-(double) n / (3.0 * fs));
        groupInE[(size_t) g] = a * groupInE[(size_t) g] + (1.0 - a) * inE / std::max (1, n);
        groupOutE[(size_t) g] = a * groupOutE[(size_t) g] + (1.0 - a) * outE / std::max (1, n);
        masterMeters.groupGainDb[(size_t) g].store (groupInE[(size_t) g] > 1e-12
                                                        ? (float) (10.0 * std::log10 ((groupOutE[(size_t) g] + 1e-20) / groupInE[(size_t) g]))
                                                        : 0.0f, std::memory_order_relaxed);
    }

    // ---- Reverb (100% wet) ------------------------------------------------
    auto mv = [&] (int slot) { return params.value (masterParam (slot)); };
    auto mpv = [&] (int slot) { return params.prevValue (masterParam (slot)); };
    ReverbParams rp;
    rp.predelayMs = mv (RevPredelay);
    rp.decayS = mv (RevDecay);
    rp.size = mv (RevSize);
    rp.hfDampHz = mv (RevHfDamp);
    rp.lowCutHz = mv (RevLowCut);
    rp.highCutHz = mv (RevHighCut);
    rp.width = mv (RevWidth);
    rp.erLevelDb = mv (RevEr);
    reverb.setParams (rp);
    reverb.process (sendSumL.data(), sendSumR.data(), wetL.data(), wetR.data(), n);
    fillRamp (tmp.data(), dbToGain (mpv (RevReturn)), dbToGain (mv (RevReturn)), n);
    for (int i = 0; i < n; ++i)
    {
        sumL[(size_t) i] += wetL[(size_t) i] * tmp[(size_t) i];
        sumR[(size_t) i] += wetR[(size_t) i] * tmp[(size_t) i];
    }

    // ---- Echo (100% wet) ----------------------------------------------------
    {
        EchoParams ep;
        ep.timeLMs = mv (EchoTimeL);
        ep.timeRMs = mv (EchoTimeR);
        ep.feedback = mv (EchoFeedback);
        ep.lowCutHz = mv (EchoLowCut);
        ep.highCutHz = mv (EchoHighCut);
        echo.setParams (ep);
        echo.process (echoSumL.data(), echoSumR.data(), wetL.data(), wetR.data(), n);
        fillRamp (tmp.data(), dbToGain (mpv (EchoReturn)), dbToGain (mv (EchoReturn)), n);
        for (int i = 0; i < n; ++i)
        {
            sumL[(size_t) i] += wetL[(size_t) i] * tmp[(size_t) i];
            sumR[(size_t) i] += wetR[(size_t) i] * tmp[(size_t) i];
        }
    }

    if (taps) pushInterleaved (taps->preMaster, sumL.data(), sumR.data(), n); // mix before glue/limiter -> analysis

    // ---- Bus compressor (glue), equal-power on/off crossfade --------------
    {
        const bool on = params.boolValue (masterParam (BcOn));
        int r = params.discreteChangeRamp (masterParam (BcOn));
        if (r > 0) rampSmoother (busOnPower, on ? 1.0f : 0.0f, r);
        const bool autoM = params.boolValue (masterParam (BcAutoMakeup));
        const float mk = autoM ? Compressor::autoMakeupDb (mv (BcThr), mv (BcRatio), mv (BcKnee)) : mv (BcMakeup);
        int ra = params.discreteChangeRamp (masterParam (BcAutoMakeup));
        rampSmoother (busMakeupDb, mk, ra > 0 ? ra : (int) (ParamEngine::kManualRampSec * fs));
        busMakeupDb.skip (n);

        CompressorParams cp;
        cp.thresholdDb = mv (BcThr);
        cp.ratio = mv (BcRatio);
        cp.attackMs = mv (BcAtt);
        cp.releaseMs = mv (BcRel);
        cp.kneeDb = mv (BcKnee);
        cp.makeupDb = busMakeupDb.getCurrentValue();
        busComp.setParams (cp);

        float grMax = 0;
        const bool idle = ! busOnPower.isSmoothing() && busOnPower.getCurrentValue() <= 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float l = sumL[(size_t) i], rr = sumR[(size_t) i];
            const float g = busComp.processSample (std::max (std::fabs (l), std::fabs (rr)));
            if (idle) continue;
            grMax = std::max (grMax, busComp.currentGainReductionDb());
            const float p = std::clamp (busOnPower.getNextValue(), 0.0f, 1.0f); // a linear ramp can step just past 0 or 1
            const float wd = std::sqrt (1.0f - p), ww = std::sqrt (p);
            sumL[(size_t) i] = l * (wd + ww * g);
            sumR[(size_t) i] = rr * (wd + ww * g);
        }
        masterMeters.busGr.store (idle ? 0.0f : grMax, std::memory_order_relaxed);
    }

    // ---- Master fader + limiter -------------------------------------------
    fillRamp (tmp.data(), dbToGain (mpv (MasterFader)), dbToGain (mv (MasterFader)), n);
    const float delivery = dbToGain (deliveryGainDb.load (std::memory_order_relaxed));
    for (int i = 0; i < n; ++i)
    {
        sumL[(size_t) i] *= tmp[(size_t) i] * delivery;
        sumR[(size_t) i] *= tmp[(size_t) i] * delivery;
    }
    {
        const bool on = params.boolValue (masterParam (LimOn));
        int r = params.discreteChangeRamp (masterParam (LimOn));
        if (r > 0) rampSmoother (limOn, on ? 1.0f : 0.0f, r);
        for (int i = 0; i < n; ++i) limW[(size_t) i] = limOn.getNextValue();
        limiter.process (sumL.data(), sumR.data(), n, mv (LimCeiling), limW.data());
        masterMeters.limGr.store (limiter.currentGainReductionDb(), std::memory_order_relaxed);
    }

    if (taps) pushInterleaved (taps->master, sumL.data(), sumR.data(), n); // processed mix -> analysis

    // ---- A/B bypass ---------------------------------------------------------
    bypassW.setTargetValue (bypass.load (std::memory_order_relaxed) ? 1.0f : 0.0f);
    const bool abActive = bypassW.isSmoothing() || bypassW.getTargetValue() > 0.0f;
    const bool match = levelMatch.load (std::memory_order_relaxed);
    const int lat = limiter.latencySamples();
    for (int i = 0; i < n; ++i)
    {
        float dl = 0, dr = 0;
        for (int t = 0; t < maxTracks(); ++t)
            if (inL[t] != nullptr && inR[t] != nullptr) { dl += inL[t][i]; dr += inR[t][i]; }
        dryL[(size_t) dryPos] = dl;
        dryR[(size_t) dryPos] = dr;
        const int rd = (dryPos - lat) & dryMask;
        dryPos = (dryPos + 1) & dryMask;
        const float xl = dryL[(size_t) rd], xr = dryR[(size_t) rd];
        procPow = powCoef * procPow + (1.0f - powCoef) * 0.5 * (sumL[(size_t) i] * sumL[(size_t) i] + sumR[(size_t) i] * sumR[(size_t) i]);
        dryPow = powCoef * dryPow + (1.0f - powCoef) * 0.5 * (xl * xl + xr * xr);
        if (! abActive) continue;
        const float target = match ? (float) juce::jlimit (0.25, 4.0, std::sqrt ((procPow + 1e-12) / (dryPow + 1e-12))) : 1.0f;
        matchGain = matchCoef * matchGain + (1.0f - matchCoef) * target;
        const float w = bypassW.getNextValue();
        sumL[(size_t) i] = (1.0f - w) * sumL[(size_t) i] + w * matchGain * xl;
        sumR[(size_t) i] = (1.0f - w) * sumR[(size_t) i] + w * matchGain * xr;
    }

    float pl = 0, pr = 0, ms = 0;
    for (int i = 0; i < n; ++i)
    {
        outL[i] = sumL[(size_t) i];
        outR[i] = sumR[(size_t) i];
        pl = std::max (pl, std::fabs (outL[i]));
        pr = std::max (pr, std::fabs (outR[i]));
        ms += 0.5f * (outL[i] * outL[i] + outR[i] * outR[i]);
    }
    masterMeters.peakL.store (pl, std::memory_order_relaxed);
    masterMeters.peakR.store (pr, std::memory_order_relaxed);
    masterMeters.rms.store (std::sqrt (ms / (float) n), std::memory_order_relaxed);
}
} // namespace im
