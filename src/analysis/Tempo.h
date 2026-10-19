#pragma once
// Tempo for the page's bars-and-beats grid: estimated from a mono signal (the drums, or the whole mix) by spectral-flux
// onsets, autocorrelation over 60-200 BPM with a preference for 90-160, then a comb search for the exact period and the
// beat phase. Bar 1 is placed on the beat (of four) with the strongest onsets.
namespace im
{
struct TempoEstimate
{
    double bpm = 120.0;
    double offsetSec = 0.0;  // where a bar starts, relative to the signal's start, in [0, bar length)
    double confidence = 0.0; // 0..1 (how periodic the onsets are)
    bool ok = false;         // false: too short or no onsets (bpm stays 120)
};

TempoEstimate estimateTempo (const float* mono, int n, double sampleRate, int beatsPerBar = 4);
} // namespace im
