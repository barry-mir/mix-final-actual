#pragma once
// Standalone multitrack source: loads a folder of stereo/mono WAVs (mono is
// duplicated to stereo; resampled to the device rate), file names become the
// default track names. Transport: play/stop/loop/seek. Sessions are swapped
// in lock-free (audio thread picks up a pending pointer, retired sessions are
// freed on the message thread).
#include "core/Params.h"
#include "core/Spsc.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>
#include <mutex>

namespace im
{
class MultitrackPlayer
{
public:
    struct Session
    {
        std::vector<juce::AudioBuffer<float>> tracks; // stereo, at sampleRate
        std::vector<std::string> names;
        int64_t length = 0;
        double sampleRate = 48000.0;
        juce::File folder;
    };

    MultitrackPlayer();
    ~MultitrackPlayer();

    // Message thread / non-RT. Returns error string (empty on success).
    juce::String loadFolder (const juce::File& dir, double targetSampleRate);
    std::unique_ptr<Session> buildSession (const juce::File& dir, double targetSampleRate, juce::String& error) const;
    void setSession (std::unique_ptr<Session> s);
    void collectGarbage();
    juce::File loadedFolder() const;
    std::vector<std::string> loadedNames() const;
    int numLoadedTracks() const noexcept { return numTracks.load(); }
    double loadedSampleRate() const noexcept { return loadedRate.load(); }

    void setPlaying (bool p) noexcept { playing.store (p); }
    bool isPlaying() const noexcept { return playing.load(); }
    void setLooping (bool l) noexcept { looping.store (l); }
    bool isLooping() const noexcept { return looping.load(); }
    void seek (double seconds) noexcept { seekTo.store (seconds); }
    double positionSec() const noexcept { return posSec.load(); }
    double lengthSec() const noexcept { return lenSec.load(); }

    // Audio thread. Fills up to kMaxTracks stereo buffers; returns mask of tracks provided.
    TrackMask fill (float* const* L, float* const* R, int n) noexcept;
    bool hasSession() const noexcept { return numTracks.load() > 0; }

private:
    Session* current = nullptr; // audio-thread owned
    std::atomic<Session*> pending { nullptr };
    SpscRing<Session*> retired;
    std::atomic<bool> playing { false }, looping { true };
    std::atomic<double> seekTo { -1.0 }, posSec { 0.0 }, lenSec { 0.0 }, loadedRate { 0.0 };
    std::atomic<int> numTracks { 0 };
    int64_t pos = 0;
    mutable std::mutex infoMutex;
    juce::File folder;
    std::vector<std::string> names;
};
} // namespace im
