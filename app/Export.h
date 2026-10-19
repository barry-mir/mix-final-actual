#pragma once
// Export: the whole song rendered offline with the current settings and clip edits (as heard, master chain included),
// 24-bit WAV or FLAC, to the file the user chose. Optionally one stem per track (each track soloed, its sends
// included, glue compressor and limiter off), in "<file name> stems/" next to the file. A separate engine renders it,
// so playback goes on.
#include <juce_core/juce_core.h>
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace im
{
class InteractiveMixerProcessor;
}

namespace mfa
{
struct ExportOptions
{
    juce::File file;                                              // .wav or .flac (the extension follows `format`)
    std::string format = "wav";                                   // "wav" | "flac"
    bool stems = false;
    double loudnessLufs = std::numeric_limits<double>::quiet_NaN(); // NaN = as mixed
    std::optional<double> knownGainDb;                            // the delivery gain for exactly these settings (live)
};

class Exporter
{
public:
    ~Exporter();
    // Message thread. False if an export is already running.
    bool start (im::InteractiveMixerProcessor& live, ExportOptions opts);

    bool running() const { return busy.load(); }
    double progress() const { return prog.load(); } // 0..1
    std::string folder() const;                    // last export's folder
    std::string file() const;                      // last export's mix file
    std::string error() const;
    double resultLufs() const { return lufsOut.load(); } // the mix file's integrated loudness

    // The default file for a project: <last export folder or Music/mix_final_ACTUAL/<name>>/<name>.wav, name = the
    // project's name (empty: the tracks folder's name).
    static juce::File defaultFile (const juce::File& tracksDir, const std::string& projectName = {});
    static void rememberFolder (const juce::File& dir); // <appdata>/settings.json

private:
    void join();
    std::thread worker;
    std::atomic<bool> busy { false };
    std::atomic<double> prog { 0.0 };
    std::atomic<double> lufsOut { 0.0 };
    mutable std::mutex m;
    std::string outDir, outFile, err;
};
} // namespace mfa
