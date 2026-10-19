#pragma once
// Playback at the export loudness. With a target set (e.g. -14 LUFS), the live engine plays with the same delivery
// gain the export would use, so what the page plays is what Export writes. The gain comes from masterTo() on an
// offline copy of the session, re-run in the background about 1.5 s after the settings or clip edits stop changing;
// the live engine glides to each new gain over 1 s. The agent's measurements stay "as mixed" (MixEngine's separate
// analysis path).
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace im
{
class InteractiveMixerProcessor;
}

namespace mfa
{
class LiveLoudness
{
public:
    ~LiveLoudness();

    // Message thread.
    void setTarget (std::optional<double> lufs, im::InteractiveMixerProcessor* live);
    std::optional<double> target() const;
    double gain() const; // the delivery gain in effect (saved in the project)
    // On reopening a project: start playback at the gain it had, before the first measurement corrects it.
    void seedGain (double gainDb, im::InteractiveMixerProcessor& live);
    void tick (im::InteractiveMixerProcessor& live); // ~4 Hz from the main loop
    // The live gain, if it was measured for exactly the current settings and this target (export reuses it).
    std::optional<double> currentGain (im::InteractiveMixerProcessor& live, double targetLufs) const;
    nlohmann::json status (im::InteractiveMixerProcessor& live) const; // {target, gain_db, state, measured_lufs, held}
    // While the page compares versions (A/B of a turn), keep the gain as it is: no re-measuring, no cached jumps.
    void setHold (bool h);
    void stop();

    // A fingerprint of everything that changes the sound: parameters, clip edits (their content), group routing
    // (message thread).
    static uint64_t settingsKey (im::InteractiveMixerProcessor& live);

private:
    void apply (im::InteractiveMixerProcessor& live);
    mutable std::mutex m;
    std::optional<double> tgt { -14.0 };
    double gainDb = 0.0;                 // in effect on the live engine
    std::optional<uint64_t> measuredKey; // settings the gain was measured for
    double measuredGain = 0.0, measuredTarget = 0.0, measuredLufs = 0.0;
    uint64_t lastKey = 0;
    double lastKeyChangeMs = 0;
    std::thread worker;
    std::atomic<bool> running { false }, cancel { false }, resultReady { false };
    uint64_t runKey = 0;
    double runTarget = 0, runGain = 0, runLufs = 0;
    bool runOk = false;
    bool hold = false;
    // The last measurements by settings: going back to a state measured before (undo, A/B) applies its gain at once.
    struct Cached
    {
        uint64_t key;
        double target, gain, lufs;
    };
    std::deque<Cached> cache;
};
} // namespace mfa
