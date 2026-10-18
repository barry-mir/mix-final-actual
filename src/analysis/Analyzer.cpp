#include "analysis/Analyzer.h"
#include "analysis/Loudness.h"
#include <cmath>

namespace im
{
Analyzer::Analyzer (AnalysisTaps& t) : juce::Thread ("IM Analysis"), taps (t)
{
    snap = std::make_shared<Snapshot>();
}

Analyzer::~Analyzer() { stop(); }

void Analyzer::start (double sampleRate, bool launchThread)
{
    stop();
    std::lock_guard<std::mutex> lk (stepMutex);
    fs = sampleRate;
    fx = std::make_unique<FeatureExtractor> (fs);
    const int win = (int) std::lround (kWindowSec * fs);
    for (auto& h : pre) h.init (win);
    for (auto& h : post) h.init (win);
    master.init (win);
    preMaster.init (win);
    for (auto& g : gr)
    {
        g.recs.assign (4096, {});
        g.pos = g.filled = 0;
    }
    drainBuf.assign ((size_t) (fs * 2.0) * 2, 0.0f);
    linL.assign ((size_t) win, 0.0f);
    linR.assign ((size_t) win, 0.0f);
    samplesSinceHop = 0;
    running = true;
    if (launchThread) startThread (juce::Thread::Priority::low);
}

void Analyzer::stop()
{
    if (isThreadRunning()) stopThread (2000);
    running = false;
}

std::shared_ptr<const Snapshot> Analyzer::latest() const
{
    std::lock_guard<std::mutex> lk (snapMutex);
    return snap;
}

void Analyzer::run()
{
    while (! threadShouldExit())
    {
        step();
        wait (25);
    }
}

bool Analyzer::step()
{
    std::lock_guard<std::mutex> lk (stepMutex);
    if (! fx) return false;
    drain();
    if (samplesSinceHop >= (int64_t) std::lround (kHopSec * fs) && master.filled > FeatureExtractor::kFftSize)
    {
        samplesSinceHop = 0;
        compute();
        return true;
    }
    return false;
}

void Analyzer::drain()
{
    auto drainRing = [this] (SpscRing<float>& ring, History& h) -> int {
        int frames = 0;
        size_t got;
        while ((got = ring.pop (drainBuf.data(), drainBuf.size())) > 0)
        {
            h.push (drainBuf.data(), (int) got / 2);
            frames += (int) got / 2;
        }
        return frames;
    };
    for (int t = 0; t < maxTracks(); ++t)
    {
        drainRing (taps.pre[(size_t) t], pre[(size_t) t]);
        drainRing (taps.post[(size_t) t], post[(size_t) t]);
        GrRecord rec;
        auto& g = gr[(size_t) t];
        while (taps.gr[(size_t) t].pop (rec))
        {
            g.recs[(size_t) g.pos] = rec;
            g.pos = (g.pos + 1) % (int) g.recs.size();
            g.filled = std::min ((int) g.recs.size(), g.filled + 1);
        }
    }
    drainRing (taps.preMaster, preMaster);
    samplesSinceHop += drainRing (taps.master, master);
}

void Analyzer::compute()
{
    auto s = std::make_shared<Snapshot>();
    s->seq = ++seq;
    s->timeSec = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    s->windowSec = kWindowSec;
    const TrackMask mask = presentMask.load();
    const int win = (int) std::lround (kWindowSec * fs);

    for (int t = 0; t < maxTracks(); ++t)
    {
        auto& ts = s->tracks[(size_t) t];
        ts.present = mask.has (t);
        if (! ts.present || pre[(size_t) t].filled < FeatureExtractor::kFftSize) continue;

        int n = pre[(size_t) t].linearize (linL, linR);
        // cheap silence check before the expensive transforms
        double e = 0;
        for (int i = 0; i < n; ++i) e += 0.5 * ((double) linL[(size_t) i] * linL[(size_t) i] + (double) linR[(size_t) i] * linR[(size_t) i]);
        const float rmsDb = (float) (10.0 * std::log10 (std::max (e / std::max (n, 1), 1e-12)));
        ts.silent = rmsDb < kSilentDb;
        if (ts.silent) continue;
        ts.pre = fx->compute (linL.data(), linR.data(), n);

        n = post[(size_t) t].linearize (linL, linR);
        ts.post = fx->compute (linL.data(), linR.data(), n);
        ts.peakDb = ts.post.peakDb;

        // GR stats over the window
        auto& g = gr[(size_t) t];
        double cSum = 0, gSum = 0, wSum = 0;
        float cMax = 0, gMax = 0;
        int samples = 0;
        for (int k = 0; k < g.filled && samples < win; ++k)
        {
            const auto& r = g.recs[(size_t) ((g.pos - 1 - k + (int) g.recs.size()) % (int) g.recs.size())];
            cSum += (double) r.compGrDb * r.numSamples;
            gSum += (double) r.gateGrDb * r.numSamples;
            wSum += r.numSamples;
            cMax = std::max (cMax, r.compGrDb);
            gMax = std::max (gMax, r.gateGrDb);
            samples += r.numSamples;
        }
        ts.compGrMean = wSum > 0 ? (float) (cSum / wSum) : 0.0f;
        ts.gateGrMean = wSum > 0 ? (float) (gSum / wSum) : 0.0f;
        ts.compGrMax = cMax;
        ts.gateGrMax = gMax;
    }

    {
        const int n = master.linearize (linL, linR);
        s->master.f = fx->compute (linL.data(), linR.data(), n);
        s->master.silent = s->master.f.rmsDb < kSilentDb;
        s->master.lufs = (float) integratedLoudness (linL.data(), linR.data(), n, fs);
        s->master.peakDb = s->master.f.peakDb;
        const int np = preMaster.linearize (linL, linR);
        if (np > FeatureExtractor::kFftSize)
        {
            const auto pf = fx->compute (linL.data(), linR.data(), np);
            s->master.cfPreDb = pf.cfDb;
            s->master.prePeakDb = pf.peakDb;
            s->master.preRmsDb = pf.rmsDb;
        }
        else
        {
            s->master.cfPreDb = s->master.f.cfDb;
            s->master.prePeakDb = s->master.f.peakDb;
            s->master.preRmsDb = s->master.f.rmsDb;
        }
    }

    // Long-term level: one 3 s window wobbles by 1-4 dB with the music (professional mixes: median 1.4 dB for vocals), so the
    // agent reads the energy average since the last parameter change. Windows that still hold pre-change audio are skipped.
    if (const auto cs = changeSerial.load (std::memory_order_relaxed); cs != seenSerial)
    {
        seenSerial = cs;
        avgCount = 0;
        std::fill (avgTrackPow.begin(), avgTrackPow.end(), 0.0);
        avgMasterPow = 0;
        avgPrePow = 0;
        ltPrePeakDb = -120;
        skipSnaps = (int) std::ceil (kWindowSec / kHopSec);
    }
    if (skipSnaps > 0)
        --skipSnaps;
    else if (! s->master.silent)
    {
        ++avgCount;
        avgMasterPow += std::pow (10.0, s->master.f.rmsDb / 10.0);
        avgPrePow += std::pow (10.0, s->master.preRmsDb / 10.0);
        ltPrePeakDb = std::max (ltPrePeakDb, s->master.prePeakDb);
        for (int t = 0; t < maxTracks(); ++t)
        {
            const auto& ts = s->tracks[(size_t) t];
            if (ts.present && ! ts.silent) avgTrackPow[(size_t) t] += std::pow (10.0, ts.post.rmsDb / 10.0);
        }
    }
    if (avgCount > 0 && avgMasterPow > 0)
    {
        s->avgSec = avgCount * kHopSec + (kWindowSec - kHopSec);
        if (avgPrePow > 0) s->master.ltCfPreDb = (float) (ltPrePeakDb - 10.0 * std::log10 (avgPrePow / avgCount));
        for (int t = 0; t < maxTracks(); ++t)
            if (avgTrackPow[(size_t) t] > 0)
                s->tracks[(size_t) t].levelAvgDb = (float) (10.0 * std::log10 (avgTrackPow[(size_t) t] / avgMasterPow));
    }

    std::lock_guard<std::mutex> lk (snapMutex);
    snap = s;
}
} // namespace im
