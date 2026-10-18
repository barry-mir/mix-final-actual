#include "Render.h"
#include "PluginProcessor.h"
#include "analysis/Loudness.h"

namespace mfa
{
using im::InteractiveMixerProcessor;

std::shared_ptr<InteractiveMixerProcessor> makeOfflineEngine (InteractiveMixerProcessor& live, double& rate, std::string& error)
{
    juce::MemoryBlock state;
    live.getStateInformation (state);
    const auto tracksDir = live.player.loadedFolder();
    rate = im::MultitrackPlayer::sessionRate (tracksDir); // the tracks' own rate: nothing to convert

    const bool before = im::gDisableBackgroundServices;
    im::gDisableBackgroundServices = true;
    auto proc = std::make_shared<InteractiveMixerProcessor>();
    im::gDisableBackgroundServices = before;
    proc->setRateAndBufferSizeDetails (rate, kRenderBlock);
    proc->prepareToPlay (rate, kRenderBlock);
    if (auto e = proc->loadSessionFolder (tracksDir); e.isNotEmpty())
    {
        error = e.toStdString();
        return nullptr;
    }
    proc->setStateInformation (state.getData(), (int) state.getSize());
    proc->player.setLooping (false);
    proc->updateTrackGroups(); // groups follow the names, which came with the state
    juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    return proc;
}

void releaseOnMessageThread (std::shared_ptr<InteractiveMixerProcessor> p)
{
    // The engine owns timers and parameter listeners: it must die on the message thread.
    juce::MessageManager::callAsync ([p = std::move (p)] {});
}

bool renderSong (InteractiveMixerProcessor& p, double rate, juce::AudioBuffer<float>& out, const std::function<bool (double)>& progress)
{
    const int bs = kRenderBlock;
    const int latency = p.engine.latencySamples();
    const auto total = (int64_t) ((p.player.lengthSec() + kTailSec) * rate) + latency;
    out.setSize (2, (int) (total - latency), false, false, true);
    out.clear();
    juce::AudioBuffer<float> io (2, bs);
    juce::MidiBuffer midi;
    p.engine.prepare (rate, bs); // clears reverb/echo/dynamics state (no tails from a previous pass), delivery gain jumps
    p.player.seek (0.0);
    p.player.setPlaying (true);
    int64_t pos = 0;
    while (pos < total)
    {
        io.clear();
        p.processBlock (io, midi);
        const int n = (int) std::min<int64_t> (bs, total - pos);
        for (int i = 0; i < n; ++i)
        {
            const int64_t o = pos + i - latency;
            if (o < 0 || o >= out.getNumSamples()) continue;
            out.setSample (0, (int) o, io.getSample (0, i));
            out.setSample (1, (int) o, io.getSample (1, i));
        }
        pos += bs;
        if ((pos / bs) % 64 == 0 && progress && ! progress ((double) pos / (double) total)) return false;
    }
    p.player.setPlaying (false);
    return true;
}

double loudnessOf (const juce::AudioBuffer<float>& b, double rate)
{
    return im::integratedLoudness (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples(), rate);
}

Mastered masterTo (InteractiveMixerProcessor& p, double rate, double target, double startGainDb, bool exact,
                   const std::function<bool (double)>& progress)
{
    Mastered m;
    p.engine.forceLimiter = true;
    double gain = std::clamp (startGainDb, -30.0, 40.0);
    const int passes = exact ? 1 : 4;
    double prevGain = 0, prevL = 0;
    for (int k = 0; k < passes; ++k)
    {
        p.engine.deliveryGainDb = (float) gain;
        auto prog = [&] (double f) { return ! progress || progress ((k + f) / passes); };
        if (! renderSong (p, rate, m.mix, prog)) return m;
        const double l = loudnessOf (m.mix, rate);
        m.gainDb = gain;
        m.lufs = l;
        if (! std::isfinite (l) || std::abs (l - target) < 0.2 || k + 1 == passes) break;
        // LU per dB of gain: 1 while the limiter idles, less as it works harder. The first step assumes 1, later steps
        // use the slope measured between the last two passes (secant).
        double slope = 1.0;
        if (k > 0 && std::abs (gain - prevGain) > 1e-3) slope = std::clamp ((l - prevL) / (gain - prevGain), 0.1, 1.0);
        prevGain = gain;
        prevL = l;
        gain = std::clamp (gain + (target - l) / slope, -30.0, 40.0);
    }
    m.ok = true;
    return m;
}
} // namespace mfa
