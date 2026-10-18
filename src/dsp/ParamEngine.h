#pragma once
// Realtime parameter engine (audio thread side).
//
// * Every parameter is read from its APVTS atomic once per block.
// * Manual edits: SmoothedValue ramp of 20 ms (continuous params), or a 20 ms
//   equal-power crossfade signalled to the strip (discrete params).
// * LLM edits arrive as a batch through a lock-free SPSC queue. All entries of
//   one batch are popped at the start of the same block, so every change of a
//   tick starts on the same sample and lasts llm_ramp_s. Ramps are linear in
//   the parameter's display domain (dB, log2(Hz), linear). A batch arriving
//   mid-ramp restarts from the current interpolated value.
// * While an LLM ramp is active, APVTS changes for that param are ignored
//   (they are the UI animation following the ramp, see ParamAnimator).
#include "core/Params.h"
#include "core/Spsc.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <memory>

namespace im
{
struct LlmChange
{
    int param = -1;
    float target = 0;   // display units (dB, Hz, ms, index...)
    float rampSec = 2;
    uint32_t batchId = 0;
};

class ParamEngine
{
public:
    static constexpr float kManualRampSec = 0.020f;

    ParamEngine()
    {
        queue.resize (4096);
        for (int i = 0; i < numParams(); ++i)
        {
            auto& s = spec (i);
            isDiscrete[(size_t) i] = s.kind != Kind::Float;
            cur[(size_t) i] = prev[(size_t) i] = lastRaw[(size_t) i] = s.def;
            sm[(size_t) i].setCurrentAndTargetValue (toRampDomain (s, s.def));
            published[(size_t) i].store (s.def);
        }
    }

    void bindRaw (int i, std::atomic<float>* p) { raw[(size_t) i] = p; }

    // Non-RT. Snap all values to their current raw values.
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        for (int i = 0; i < numParams(); ++i)
        {
            const float v = raw[(size_t) i] ? raw[(size_t) i]->load() : spec (i).def;
            lastRaw[(size_t) i] = cur[(size_t) i] = prev[(size_t) i] = v;
            sm[(size_t) i].setCurrentAndTargetValue (toRampDomain (spec (i), v));
            llmActive[(size_t) i].store (false);
            discreteRemaining[(size_t) i] = 0;
            published[(size_t) i].store (v);
        }
    }

    // ---- message thread ---------------------------------------------------
    bool pushLlmBatch (const LlmChange* changes, size_t n) { return queue.pushAll (changes, n); }

    // ---- audio thread -----------------------------------------------------
    void beginBlock (int numSamples) noexcept
    {
        ++blockCounter;
        lastBlockSize = numSamples;

        // 1. LLM batches (all entries of a batch become visible atomically).
        LlmChange tmp[128];
        size_t got;
        while ((got = queue.pop (tmp, 128)) > 0)
            for (size_t k = 0; k < got; ++k)
                applyLlm (tmp[k]);

        // 2. Manual / host-automation edits.
        const int manualSamples = juce::jmax (1, (int) std::lround (kManualRampSec * fs));
        for (size_t i = 0; i < (size_t) numParams(); ++i)
        {
            if (raw[i] == nullptr || llmActive[i].load (std::memory_order_relaxed) || uiAnim[i].load (std::memory_order_relaxed))
                continue;
            const float r = raw[i]->load (std::memory_order_relaxed);
            if (r == lastRaw[i]) continue;
            lastRaw[i] = r;
            if (isDiscrete[i])
            {
                discreteVal (i, r, manualSamples);
            }
            else
            {
                startRamp (i, r, manualSamples);
            }
        }

        // 3. Advance smoothing to the end of this block.
        for (size_t i = 0; i < (size_t) numParams(); ++i)
        {
            prev[i] = cur[i];
            if (isDiscrete[i])
            {
                if (llmActive[i].load (std::memory_order_relaxed))
                {
                    discreteRemaining[i] -= numSamples;
                    if (discreteRemaining[i] <= 0) finishLlm (i);
                }
                continue;
            }
            auto& s = sm[i];
            if (s.isSmoothing())
            {
                s.skip (numSamples);
                cur[i] = fromRampDomain (spec ((int) i), s.getCurrentValue());
                published[i].store (cur[i], std::memory_order_relaxed);
                if (! s.isSmoothing() && llmActive[i].load (std::memory_order_relaxed))
                    finishLlm (i);
            }
        }
    }

    float value (int i) const noexcept { return cur[(size_t) i]; }
    float prevValue (int i) const noexcept { return prev[(size_t) i]; }
    bool boolValue (int i) const noexcept { return cur[(size_t) i] > 0.5f; }
    int intValue (int i) const noexcept { return (int) std::lround (cur[(size_t) i]); }
    // Ramp length in samples if this discrete param changed in the current block, else -1.
    int discreteChangeRamp (int i) const noexcept
    {
        return changedBlock[(size_t) i] == blockCounter ? changedRamp[(size_t) i] : -1;
    }
    bool isSmoothing (int i) const noexcept { return sm[(size_t) i].isSmoothing(); }

    // ---- UI / animator ----------------------------------------------------
    float publishedValue (int i) const noexcept { return published[(size_t) i].load (std::memory_order_relaxed); }
    bool isLlmActive (int i) const noexcept { return llmActive[(size_t) i].load(); }
    std::atomic<bool>& uiAnimFlag (int i) noexcept { return uiAnim[(size_t) i]; }
    std::atomic<uint32_t>& consumedBatch (int i) noexcept { return consumed[(size_t) i]; }

    // ---- diagnostics (tests) ---------------------------------------------
    int64_t currentBlock() const noexcept { return blockCounter; }
    int64_t rampStartBlock (int i) const noexcept { return startBlock[(size_t) i]; }
    int64_t rampEndBlock (int i) const noexcept { return endBlock[(size_t) i]; }

private:
    void applyLlm (const LlmChange& c) noexcept
    {
        if (c.param < 0 || c.param >= numParams()) return;
        const auto i = (size_t) c.param;
        const int ramp = juce::jmax (1, (int) std::lround (c.rampSec * fs));
        lastRaw[i] = c.target;
        llmActive[i].store (true, std::memory_order_relaxed);
        if (isDiscrete[i])
        {
            discreteVal (i, c.target, ramp);
            discreteRemaining[i] = ramp;
        }
        else
            startRamp (i, c.target, ramp);
        startBlock[i] = blockCounter;
        endBlock[i] = -1;
        consumed[i].store (c.batchId, std::memory_order_release);
    }

    void finishLlm (size_t i) noexcept
    {
        llmActive[i].store (false, std::memory_order_release);
        endBlock[i] = blockCounter;
    }

    void startRamp (size_t i, float target, int samples) noexcept
    {
        auto& s = sm[i];
        const float now = s.getCurrentValue();
        s.setCurrentAndTargetValue (now); // keep current; reset() would otherwise jump to old target
        s.reset (samples);
        s.setTargetValue (toRampDomain (spec ((int) i), target));
    }

    void discreteVal (size_t i, float v, int rampSamples) noexcept
    {
        cur[i] = v;
        published[i].store (v, std::memory_order_relaxed);
        changedBlock[i] = blockCounter;
        changedRamp[i] = rampSamples;
    }

    double fs = 48000.0;
    int lastBlockSize = 0;
    int64_t blockCounter = 0;
    SpscRing<LlmChange> queue;

    std::vector<std::atomic<float>*> raw = std::vector<std::atomic<float>*> ((size_t) numParams());
    std::vector<bool> isDiscrete = std::vector<bool> ((size_t) numParams());
    std::vector<float> lastRaw = std::vector<float> ((size_t) numParams()), cur = std::vector<float> ((size_t) numParams()), prev = std::vector<float> ((size_t) numParams());
    std::vector<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>> sm = std::vector<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>> ((size_t) numParams());
    std::vector<int> discreteRemaining = std::vector<int> ((size_t) numParams()), changedRamp = std::vector<int> ((size_t) numParams());
    std::vector<int64_t> changedBlock = std::vector<int64_t> ((size_t) numParams()), startBlock = std::vector<int64_t> ((size_t) numParams()), endBlock = std::vector<int64_t> ((size_t) numParams());
    std::vector<std::atomic<bool>> llmActive = std::vector<std::atomic<bool>> ((size_t) numParams()), uiAnim = std::vector<std::atomic<bool>> ((size_t) numParams());
    std::vector<std::atomic<uint32_t>> consumed = std::vector<std::atomic<uint32_t>> ((size_t) numParams());
    std::vector<std::atomic<float>> published = std::vector<std::atomic<float>> ((size_t) numParams());
};
} // namespace im
