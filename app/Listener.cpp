#include "Listener.h"
#include "Render.h"
#include <condition_variable>

namespace mfa
{
using im::InteractiveMixerProcessor;

std::pair<double, double> analysisRange (InteractiveMixerProcessor& live, bool whole)
{
    const double len = live.player.lengthSec();
    if (whole) return { 0.0, len };
    const double a = live.player.loopStartSec(), b = std::min (live.player.loopEndSec(), len);
    if (b - a >= 2.0 && a >= 0.0) return { a, b };
    return { live.player.excerptStartSec(), std::min (live.player.excerptEndSec(), len) };
}

void Listener::attach (InteractiveMixerProcessor& live, const std::atomic<bool>& wholeSong)
{
    live.offlineListener = [this, &live, &wholeSong] (const std::function<bool()>& stop, InteractiveMixerProcessor::Listened& out) {
        return listen (live, wholeSong, stop, out);
    };
}

void Listener::release()
{
    std::lock_guard<std::mutex> lk (m);
    engine.reset(); // on the message thread: the engine owns timers and parameter listeners
}

bool Listener::listen (InteractiveMixerProcessor& live, const std::atomic<bool>& wholeSong, const std::function<bool()>& stop,
                       InteractiveMixerProcessor::Listened& out)
{
    std::lock_guard<std::mutex> lk (m);
    const auto [a, b] = analysisRange (live, wholeSong.load());
    live.listenA = a;
    live.listenB = b;
    live.listenNeed = b - a;
    live.listenHave = 0.0;
    struct Job
    {
        std::mutex m;
        std::condition_variable cv;
        bool done = false, ok = false;
    };
    // 1. on the message thread: build the engine once, then copy the live settings, clip edits and names into it
    auto job = std::make_shared<Job>();
    juce::MessageManager::callAsync ([this, job, &live] {
        std::string err;
        if (! engine) engine = makeOfflineEngine (live, rate, err);
        else
        {
            juce::MemoryBlock state;
            live.getStateInformation (state);
            engine->setStateInformation (state.getData(), (int) state.getSize());
            engine->updateTrackGroups();
        }
        if (engine) // the agent measures the mix, not what is soloed for listening
            for (int t = 0; t < engine->player.numLoadedTracks(); ++t) engine->setParamValue (im::trackParam (t, im::Solo), 0.0f);
        std::lock_guard<std::mutex> g (job->m);
        job->done = true;
        job->ok = engine != nullptr;
        job->cv.notify_all();
    });
    {
        std::unique_lock<std::mutex> g (job->m);
        while (! job->done)
            if (! job->cv.wait_for (g, std::chrono::milliseconds (100), [&] { return job->done; }) && stop && stop()) return false;
        if (! job->ok) return false;
    }
    auto& p = *engine;

    // 2. here (the agent's thread): render the range. The last 3 s of the range first (fills the analysis window), then
    // the whole range once: the long-term averages cover the range, the 3 s features its end, as when the first mix listens.
    const double range = std::max (0.1, b - a), warm = std::min (3.0, range);
    const int bs = kRenderBlock;
    p.engine.prepare (rate, bs);
    p.analyzer.start (rate, false);
    p.player.setLoopRegion (a, b);
    p.player.setLooping (true);
    p.player.seek (b - warm);
    p.player.setPlaying (true);
    juce::AudioBuffer<float> io (2, bs);
    juce::MidiBuffer midi;
    const auto total = (int64_t) ((warm + range) * rate), warmFrames = (int64_t) (warm * rate);
    p.analyzer.noteParamChange(); // the averages restart once the 3 s window holds only this render: on the range itself
    for (int64_t pos = 0; pos < total; pos += bs)
    {
        io.clear();
        p.processBlock (io, midi);
        p.analyzer.step();
        live.listenHave = std::clamp ((double) (pos - warmFrames) / rate, 0.0, range);
        if ((pos / bs) % 32 == 0 && stop && stop())
        {
            p.player.setPlaying (false);
            return false;
        }
    }
    // The whole song ends in its outro (often silence), and the short-window features describe the render's last 3 s:
    // close with 3 s of the excerpt, where every instrument plays (the long-term averages still cover the whole song).
    if (wholeSong.load())
    {
        const double ex = std::min (live.player.excerptEndSec(), b);
        p.player.seek (std::max (a, ex - 3.0));
        for (int64_t pos = 0; pos < (int64_t) (3.0 * rate); pos += bs)
        {
            io.clear();
            p.processBlock (io, midi);
            p.analyzer.step();
        }
    }
    p.player.setPlaying (false);
    live.listenHave = range;
    out.snap = p.analyzer.latest();
    for (int g = 0; g < im::kNumGroupBuses; ++g)
    {
        out.busGr[(size_t) g] = p.engine.masterMeters.groupGr[(size_t) g].load();
        out.busGain[(size_t) g] = p.engine.masterMeters.groupGainDb[(size_t) g].load();
    }
    return out.snap != nullptr;
}
} // namespace mfa
