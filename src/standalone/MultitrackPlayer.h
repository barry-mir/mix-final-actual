#pragma once
// Multitrack source: plays a folder of audio files, one per track (file names become the track names).
//
// Tracks are never loaded into memory: WAV/AIFF files at the session rate are memory-mapped and read in place (the OS
// pages them in; a prefetch thread touches the pages ahead of the playhead so the audio thread does not wait), and
// other formats or rates are converted once to a float WAV in the app's cache, which is mapped the same way. So the
// RAM a session needs does not grow with its track count or length.
//
// On load (as the evaluation sessions were made): one common gain puts the unity sum of the excerpt at -23 LUFS, and
// the excerpt is the 30 s where the quietest instrument group is loudest. Transport: play/stop/loop region/seek.
// Sessions are swapped in lock-free (the audio thread picks up a pending pointer). A session is small (open mapped
// files), so every session loaded stays alive until the player is destroyed: nothing is freed under the audio thread.
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
    struct Track
    {
        std::unique_ptr<juce::MemoryMappedAudioFormatReader> reader; // at the session rate
        int channels = 1;                                            // 1 or 2 (more: the first two)
        int64_t bytesPerFrame = 4;
    };

    struct Session
    {
        std::vector<Track> tracks;
        std::vector<std::string> names;
        int64_t length = 0; // frames
        double sampleRate = 48000.0;
        juce::File folder;
        int64_t excerptStart = 0, excerptEnd = 0; // frames
        double gainDb = 0.0;                      // applied to every track
        float gain = 1.0f;
    };

    MultitrackPlayer();
    ~MultitrackPlayer();

    // The audio files of a session folder, in track order (also what decides the track count, see setMaxTracks).
    static juce::Array<juce::File> trackFiles (const juce::File& dir);
    // The sample rate most of the folder's tracks have (open the audio device at it to avoid conversions).
    static double sessionRate (const juce::File& dir);

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

    // Audio thread. Fills one stereo buffer per track; returns the tracks provided.
    TrackMask fill (float* const* L, float* const* R, int n) noexcept;
    bool hasSession() const noexcept { return numTracks.load() > 0; }

private:
    class Prefetcher;
    std::unique_ptr<Prefetcher> prefetch;
    Session* current = nullptr;                 // audio thread
    std::atomic<Session*> pending { nullptr };  // set by setSession, taken by the audio thread
    std::atomic<Session*> active { nullptr };   // what plays (read by the prefetcher)
    std::vector<std::unique_ptr<Session>> owned; // every session loaded (guarded by infoMutex)
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
