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
#include <string>
#include <vector>

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

    // A clip on the timeline (seconds): `len` seconds of a file from `src`, starting at `pos`; gain in dB; equal-power
    // fades in seconds. The file is the clip's track's own unless `srcTrack` names another track (a clip pasted onto
    // another track plays that track's audio through this track's strip). Clips on a track may overlap: they sum, each
    // with its own fades (a crossfade is two overlapping clips whose fades span the overlap). The default edit
    // (identity) is one region per track covering its whole file at 0, which plays exactly like the file.
    struct Region
    {
        double pos = 0, src = 0, len = 0, gainDb = 0, fadeIn = 0, fadeOut = 0;
        int srcTrack = -1; // -1 = the track the clip is on
        bool operator== (const Region&) const = default;
    };
    using Regions = std::vector<Region>;
    static constexpr int kMaxRegions = 512;
    // Checks and tidies one track's regions: "" when valid (then `r` is sorted by position, srcTrack resolved to a track
    // index, fades clamped to half the region), else what is wrong. fileLenSec[k] = track k's file length.
    static std::string normaliseRegions (Regions& r, int ownTrack, const std::vector<double>& fileLenSec);

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

    // ---- clips (message thread) ----
    std::vector<Regions> edits() const;          // per loaded track (identity regions where not edited)
    int editsVersion() const noexcept { return editsVer.load(); }
    bool editsAreIdentity() const;
    std::string setTrackRegions (int track, Regions r); // "" or the error (nothing changed then)
    // saved state; invalid regions dropped; empty = identity unless cleared[t] (every clip deleted: the track is silent)
    void setAllEdits (const std::vector<Regions>& all, const std::vector<char>& cleared = {});
    double trackLengthSec (int track) const;            // the file's length
    // Any thread, not real time: the mono sum of some tracks' files (no edits, no session gain) over [start, start+len).
    std::vector<float> mixdown (const std::vector<int>& tracks, double startSec, double lenSec) const;

    // ---- waveform overview (any thread) ----
    // One value per peaksBlock() frames: round(255 * sqrt(max |sample| of both channels)). Built once per session in
    // the background; empty until peaksReady().
    int peaksBlock() const noexcept { return peakBlock.load(); }
    bool peaksReady() const noexcept { return peaksDone.load(); }
    std::vector<uint8_t> peaks (int track) const;

    // Audio thread. Fills one stereo buffer per track; returns the tracks provided.
    TrackMask fill (float* const* L, float* const* R, int n) noexcept;
    bool hasSession() const noexcept { return numTracks.load() > 0; }

private:
    struct RtRegion
    {
        int64_t pos, src, len, fadeIn, fadeOut;
        float gain;
        int srcTrack;
    };
    struct RtEdits // immutable once published; kept alive until the player is destroyed
    {
        const Session* session = nullptr;
        bool identity = true;
        int64_t end = 0; // song length in frames
        std::vector<std::vector<RtRegion>> tracks;
    };
    void publishEdits (const Session* s); // message thread, infoMutex held
    static std::vector<double> fileLengths (const Session& s);
    // Audio thread: the clips of one track over [start, start + n), summed (uses the scratch buffers).
    void renderRegions (const Session& s, const std::vector<RtRegion>& regs, int64_t start, int n, float* L, float* R) noexcept;
    static constexpr int kScratch = 4096;
    std::vector<float> scratchL = std::vector<float> (kScratch), scratchR = std::vector<float> (kScratch);

    class Prefetcher;
    class PeakBuilder;
    std::unique_ptr<Prefetcher> prefetch;
    std::unique_ptr<PeakBuilder> peakBuilder;
    std::vector<Regions> editSec;                   // what the user set (guarded by infoMutex); empty = identity...
    std::vector<char> clearedSec;                   // ...unless the user deleted every clip of that track (guarded by infoMutex)
    std::vector<std::unique_ptr<RtEdits>> rtOwned;  // guarded by infoMutex
    std::atomic<RtEdits*> rtEdits { nullptr };
    std::atomic<int> editsVer { 0 };
    std::vector<std::vector<uint8_t>> peakData;     // guarded by infoMutex
    std::atomic<int> peakBlock { 256 };
    std::atomic<bool> peaksDone { false };
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
