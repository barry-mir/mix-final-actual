#include "LiveLoudness.h"
#include "PluginProcessor.h"
#include "Render.h"

namespace mfa
{
using im::InteractiveMixerProcessor;

namespace
{
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

bool isSolo (int i)
{
    const auto& s = im::spec (i);
    return s.track >= 0 && i == im::trackParam (s.track, im::Solo);
}

bool anySolo (InteractiveMixerProcessor& live)
{
    for (int t = 0; t < live.player.numLoadedTracks(); ++t)
        if (live.rawValue (im::trackParam (t, im::Solo)) > 0.5f) return true;
    return false;
}
} // namespace

LiveLoudness::~LiveLoudness() { stop(); }

void LiveLoudness::stop()
{
    cancel = true;
    if (worker.joinable()) worker.join();
}

uint64_t LiveLoudness::settingsKey (InteractiveMixerProcessor& live)
{
    // FNV-1a over every parameter value (solo excluded: soloing to listen must not re-master the song), the clip
    // edits' version and the group routing.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h] (const void* d, size_t n) {
        auto* b = static_cast<const unsigned char*> (d);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    for (int i = 0; i < im::numParams(); ++i)
    {
        if (isSolo (i)) continue;
        const float v = live.rawValue (i);
        mix (&v, sizeof v);
    }
    const int ev = live.player.editsVersion();
    mix (&ev, sizeof ev);
    for (int t = 0; t < live.player.numLoadedTracks(); ++t)
    {
        const int g = live.engine.trackGroup[(size_t) t].load();
        mix (&g, sizeof g);
    }
    return h;
}

double LiveLoudness::gain() const
{
    std::lock_guard<std::mutex> lk (m);
    return tgt ? gainDb : 0.0;
}

void LiveLoudness::seedGain (double g, InteractiveMixerProcessor& live)
{
    std::lock_guard<std::mutex> lk (m);
    if (! tgt) return;
    gainDb = std::clamp (g, -30.0, 40.0);
    apply (live);
}

void LiveLoudness::apply (InteractiveMixerProcessor& live)
{
    // (m held)
    live.engine.separateAnalysis = tgt.has_value();
    live.engine.forceLimiter = tgt.has_value();
    live.engine.deliveryGainDb = tgt ? (float) gainDb : 0.0f;
}

void LiveLoudness::setTarget (std::optional<double> lufs, InteractiveMixerProcessor* live)
{
    std::lock_guard<std::mutex> lk (m);
    // A first guess at once (the gain moved by the change of target); the measurement then corrects it.
    if (lufs && tgt) gainDb += *lufs - *tgt;
    else if (lufs && measuredKey) gainDb = measuredGain + *lufs - measuredTarget;
    gainDb = std::clamp (gainDb, -30.0, 40.0);
    tgt = lufs;
    if (running && (! lufs || *lufs != runTarget)) cancel = true;
    if (live) apply (*live);
}

std::optional<double> LiveLoudness::target() const
{
    std::lock_guard<std::mutex> lk (m);
    return tgt;
}

void LiveLoudness::tick (InteractiveMixerProcessor& live)
{
    const auto key = settingsKey (live);
    const double now = nowMs();
    std::unique_lock<std::mutex> lk (m);
    if (key != lastKey)
    {
        lastKey = key;
        lastKeyChangeMs = now;
        if (running && runKey != key) cancel = true; // stale: stop it, measure again once things settle
    }
    if (resultReady)
    {
        resultReady = false;
        lk.unlock();
        if (worker.joinable()) worker.join();
        lk.lock();
        if (runOk && ! cancel && tgt && runTarget == *tgt)
        {
            gainDb = measuredGain = runGain;
            measuredKey = runKey;
            measuredTarget = runTarget;
            measuredLufs = runLufs;
            apply (live);
        }
        cancel = false;
    }
    if (running || ! tgt) return;
    const bool stale = ! measuredKey || *measuredKey != key || measuredTarget != *tgt;
    if (! stale || now - lastKeyChangeMs < 1500.0) return;

    // Start a measurement on an offline copy (built here, on the message thread).
    double rate = 48000.0;
    std::string err;
    auto proc = makeOfflineEngine (live, rate, err);
    if (! proc) return;
    for (int t = 0; t < proc->player.numLoadedTracks(); ++t) proc->setParamValue (im::trackParam (t, im::Solo), 0.0f);
    if (worker.joinable())
    {
        lk.unlock();
        worker.join();
        lk.lock();
    }
    running = true;
    cancel = false;
    runKey = key;
    runTarget = *tgt;
    const double start = gainDb;
    worker = std::thread ([this, proc, rate, start] {
        auto r = masterTo (*proc, rate, runTarget, start, false, [this] (double) { return ! cancel.load(); });
        {
            std::lock_guard<std::mutex> g (m);
            runOk = r.ok && std::isfinite (r.lufs);
            runGain = r.gainDb;
            runLufs = r.lufs;
        }
        releaseOnMessageThread (proc);
        resultReady = true;
        running = false;
    });
}

std::optional<double> LiveLoudness::currentGain (InteractiveMixerProcessor& live, double targetLufs) const
{
    const auto key = settingsKey (live);
    if (anySolo (live)) return std::nullopt; // the export renders what is soloed; measured without solo
    std::lock_guard<std::mutex> lk (m);
    if (running || ! measuredKey || *measuredKey != key || measuredTarget != targetLufs) return std::nullopt;
    return gainDb;
}

nlohmann::json LiveLoudness::status (InteractiveMixerProcessor& live) const
{
    const auto key = settingsKey (live);
    std::lock_guard<std::mutex> lk (m);
    if (! tgt) return { { "target", nullptr }, { "gain_db", 0.0 }, { "state", "off" }, { "measured_lufs", nullptr } };
    const bool ready = ! running && measuredKey && *measuredKey == key && measuredTarget == *tgt;
    auto num = [&] (double v) { return measuredKey && std::isfinite (v) ? nlohmann::json (v) : nlohmann::json (nullptr); };
    return { { "target", *tgt }, { "gain_db", gainDb }, { "state", ready ? "ready" : "measuring" },
             { "measured_lufs", num (measuredLufs) } };
}
} // namespace mfa
