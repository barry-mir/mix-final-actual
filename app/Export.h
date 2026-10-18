#pragma once
// Export: the whole song rendered offline with the current settings (as heard, master chain included), 24-bit WAV,
// into "<tracks folder>/mix_final_ACTUAL exports/<date time>/". Optionally one stem per track (each track soloed,
// its sends included, glue compressor and limiter off). A separate engine renders it, so playback goes on.
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace im
{
class InteractiveMixerProcessor;
}

namespace mfa
{
class Exporter
{
public:
    ~Exporter();
    // Message thread. False if an export is already running.
    bool start (im::InteractiveMixerProcessor& live, bool stems);

    bool running() const { return busy.load(); }
    double progress() const { return prog.load(); } // 0..1
    std::string folder() const;                    // last export's folder
    std::string error() const;

private:
    void join();
    std::thread worker;
    std::atomic<bool> busy { false };
    std::atomic<double> prog { 0.0 };
    mutable std::mutex m;
    std::string outDir, err;
};
} // namespace mfa
