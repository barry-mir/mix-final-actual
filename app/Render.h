#pragma once
// Offline renders of the whole song, shared by Export and the live loudness measurer, so both master the mix the same
// way: the delivery gain for a loudness target is found by one function, and live playback uses the gain it found.
#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>
#include <limits>
#include <memory>
#include <string>

namespace im
{
class InteractiveMixerProcessor;
}

namespace mfa
{
constexpr double kTailSec = 3.0; // reverb and echo ring-out after the last note
constexpr int kRenderBlock = 512;

// Message thread: a fresh offline engine with the live engine's settings and clip edits, at the tracks' own rate.
// Release it on the message thread too (releaseOnMessageThread).
std::shared_ptr<im::InteractiveMixerProcessor> makeOfflineEngine (im::InteractiveMixerProcessor& live, double& rate, std::string& error);
void releaseOnMessageThread (std::shared_ptr<im::InteractiveMixerProcessor> p);

// Worker thread. The whole song from the top plus the tail, latency removed. progress(fraction) returns false to
// cancel (then the result is false).
bool renderSong (im::InteractiveMixerProcessor& p, double rate, juce::AudioBuffer<float>& out, const std::function<bool (double)>& progress);

double loudnessOf (const juce::AudioBuffer<float>& b, double rate);

struct Mastered
{
    bool ok = false;      // false: cancelled
    double gainDb = 0;    // the delivery gain (before the limiter, limiter on)
    double lufs = std::numeric_limits<double>::quiet_NaN(); // of the rendered result
    juce::AudioBuffer<float> mix;
};

// Worker thread. The delivery gain that brings the song to `targetLufs`: renders at startGainDb, corrects up to three times
// (the limiter makes it nonlinear), stops within 0.2 LU. exact = startGainDb is already the answer for these settings:
// one render. progress spans all passes.
Mastered masterTo (im::InteractiveMixerProcessor& p, double rate, double targetLufs, double startGainDb, bool exact,
                   const std::function<bool (double)>& progress);
} // namespace mfa
