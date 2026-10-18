#include "FirstMix.h"
#include "PluginProcessor.h"

namespace mfa
{
using im::InteractiveMixerProcessor;

std::string runFirstMix (const juce::File& tracksDir, int turns, double listenSec, FirstMixProgress& progress,
                         juce::MemoryBlock& stateOut, const std::atomic<bool>& quit)
{
    // No analysis or agent threads: this engine is driven block by block, and its turns run synchronously here.
    const bool before = im::gDisableBackgroundServices;
    im::gDisableBackgroundServices = true;
    auto proc = std::make_unique<InteractiveMixerProcessor>();
    im::gDisableBackgroundServices = before;
    auto& p = *proc;

    const double sr = 48000.0;
    const int bs = 512;
    p.setRateAndBufferSizeDetails (sr, bs);
    p.prepareToPlay (sr, bs);
    if (auto err = p.loadSessionFolder (tracksDir); err.isNotEmpty()) return err.toStdString();
    p.player.setLooping (true);
    p.player.setPlaying (true);
    {
        auto s = p.store.settings();
        s.llmRampS = 0.5; // offline: the moves settle within the next listen window
        p.store.setSettings (s);
    }
    p.agent.setTurnTimeout (3600.0);

    juce::AudioBuffer<float> buf (2 * im::kMaxTracks, bs);
    juce::MidiBuffer midi;
    auto runAudio = [&] (double sec) {
        const int blocks = (int) std::ceil (sec * sr / bs);
        for (int b = 0; b < blocks && ! quit; ++b)
        {
            buf.clear();
            p.processBlock (buf, midi);
            p.analyzer.step();
            if (b % 8 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
        }
    };

    progress.turns = turns; // (also set by the caller, so the page knows the count before the session loads)
    runAudio (0.2);
    for (int k = 1; k <= turns && ! quit; ++k)
    {
        runAudio (listenSec); // what the agent "hears" before this turn (>= 4 s fills the 3 s analysis window)
        progress.turn = k;
        p.agent.runTick ({ "auto", {}, "first_mix" });
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20); // apply (message thread)
        runAudio (1.0);                                                  // let the ramp finish
    }
    if (quit) return "stopped";
    p.player.setPlaying (false);
    p.getStateInformation (stateOut);
    progress.done = true;
    return {};
}
} // namespace mfa
