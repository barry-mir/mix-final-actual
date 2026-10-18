#pragma once
// Echo: the shared stereo delay return (tracks send to it like to the reverb).
//
// Two delay lines with separate left/right times, so a centred vocal sent to it comes back as repeats on both sides at
// different moments: width and depth without the wash of a reverb. Each repeat is filtered (low cut + high cut inside
// the feedback loop), so repeats get thinner and darker like a tape or analog delay and stay out of the way of the dry
// voice. Delay times are smoothed (~80 ms) so moving them glides instead of clicking. Output is 100% wet.
#include "dsp/Filters.h"
#include <array>
#include <vector>

namespace im
{
struct EchoParams
{
    float timeLMs = 250, timeRMs = 375;
    float feedback = 0.25f;  // 0..0.9, per repeat
    float lowCutHz = 250, highCutHz = 6000;
};

class Echo
{
public:
    static constexpr float kMaxTimeMs = 1600.0f;

    void prepare (double sampleRate);
    void reset();
    void setParams (const EchoParams& p) noexcept;
    // in: the send sum; out: the wet return. n samples.
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;

private:
    float read (int ch, float d) const noexcept;

    double fs = 48000.0;
    int size = 0, mask = 0, writePos = 0;
    std::array<std::vector<float>, 2> buf;
    std::array<float, 2> timeSm { 0, 0 }, target { 0, 0 };
    float feedback = 0.25f, smCoef = 0;
    StereoBiquad lowCut, highCut;
    bool primed = false;
};
} // namespace im
