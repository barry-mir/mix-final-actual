#pragma once
// Multitrack source: loads a folder of audio files (mono is duplicated to stereo; resampled to the device rate);
// file names become the track names. On load (as the evaluation sessions were made): one common gain puts the unity
// sum of the excerpt at -23 LUFS, and the excerpt is the 30 s where the quietest instrument group is loudest. Transport: play/stop/loop/seek. Sessions are swapped
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
        int64_t excerptStart = 0, excerptEnd = 0; // samples
        double gainDb = 0.0;                      // applied to every track
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
    // Loop region in seconds (end <= start = whole file). Playback wraps inside it when looping.
    void setLoopRegion (double startSec, double endSec) noexcept { loopA.store (startSec); loopB.store (endSec); }
    double loopStartSec() const noexcept { return loopA.load(); }
    double loopEndSec() const noexcept { return loopB.load(); }
    double excerptStartSec() const noexcept { return excA.load(); }
    double excerptEndSec() const noexcept { return excB.load(); }
    double inputGainDb() const noexcept { return gainDbNow.load(); }

    // Audio thread. Fills up to kMaxTracks stereo buffers; returns mask of tracks provided.
    TrackMask fill (float* const* L, float* const* R, int n) noexcept;
    bool hasSession() const noexcept { return numTracks.load() > 0; }

private:
    Session* current = nullptr; // audio-thread owned
    std::atomic<Session*> pending { nullptr };
    SpscRing<Session*> retired;
    std::atomic<bool> playing { false }, looping { true };
    std::atomic<double> seekTo { -1.0 }, posSec { 0.0 }, lenSec { 0.0 }, loadedRate { 0.0 };
    std::atomic<double> loopA { 0.0 }, loopB { 0.0 }, excA { 0.0 }, excB { 0.0 }, gainDbNow { 0.0 };
    std::atomic<int> numTracks { 0 };
    int64_t pos = 0;
    mutable std::mutex infoMutex;
    juce::File folder;
    std::vector<std::string> names;
};
} // namespace im
