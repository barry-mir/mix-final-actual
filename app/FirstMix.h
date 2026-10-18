#pragma once
// The first mix, made before the page opens: an offline engine plays the session excerpt faster than real time, and
// the coding agent answers `turns` turns through the bridge (the paper's method: turn 1 builds the mix, later turns
// read what the earlier moves did and fix what is flagged). The result is the engine state, which the live engine
// then loads.
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <string>

namespace mfa
{
struct FirstMixProgress
{
    std::atomic<int> turn { 0 };  // turn being answered (1-based), 0 = not started
    std::atomic<int> turns { 5 };
    std::atomic<bool> done { false };
};

// Returns "" and fills stateOut (InteractiveMixerProcessor state) on success, else the error.
std::string runFirstMix (const juce::File& tracksDir, int turns, double listenSec, FirstMixProgress& progress,
                         juce::MemoryBlock& stateOut, const std::atomic<bool>& quit);
} // namespace mfa
