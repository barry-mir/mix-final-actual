#pragma once
// Offline listening for live agent turns: before each turn the console renders the analysis range (the cycle region,
// the excerpt, or the whole song) in a second engine with the live settings and clip edits, faster than real time, and
// the agent's measurements come from that render. Nothing plays and the user's playback is untouched. The engine is
// built once (tracks memory-mapped) and reused; each turn copies the live state into it.
#include "PluginProcessor.h"
#include <memory>
#include <mutex>
#include <utility>

namespace mfa
{
// What the next agent turn analyzes: the whole song, else the cycle (loop) region when it is at least 2 s, else the
// excerpt. Seconds.
std::pair<double, double> analysisRange (im::InteractiveMixerProcessor& live, bool whole);

class Listener
{
public:
    // The hook for live.offlineListener. wholeSong is read at each turn.
    void attach (im::InteractiveMixerProcessor& live, const std::atomic<bool>& wholeSong);
    void release(); // message thread, after the agent has stopped

private:
    bool listen (im::InteractiveMixerProcessor& live, const std::atomic<bool>& wholeSong, const std::function<bool()>& stop,
                 im::InteractiveMixerProcessor::Listened& out);
    std::mutex m;
    std::shared_ptr<im::InteractiveMixerProcessor> engine;
    double rate = 48000.0;
};
} // namespace mfa
