#pragma once
// What the web server and the MCP endpoints read about this app instance: one session, first mix then live.
#include "AutoAgent.h"
#include "Export.h"
#include "FirstMix.h"
#include "LiveLoudness.h"
#include <nlohmann/json.hpp>
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
// The song's tempo grid (bars and beats in the page): detected at load, or set by the user; saved in the project.
struct TempoState
{
    double bpm = 120.0;
    int beatsPerBar = 4;
    double offsetSec = 0.0; // where bar 1 starts
    bool detected = false;  // true while the values are the detector's (a user edit makes it false)
    double confidence = 0.0;
    std::string state = "none"; // "detecting" | "ready" | "none" (nothing detected, the defaults)
    nlohmann::json toJson() const
    {
        return { { "bpm", bpm }, { "beats_per_bar", beatsPerBar }, { "offset_s", offsetSec }, { "detected", detected },
                 { "confidence", confidence }, { "state", state } };
    }
    void fromJson (const nlohmann::json& j)
    {
        bpm = std::clamp (j.value ("bpm", 120.0), 20.0, 400.0);
        beatsPerBar = std::clamp (j.value ("beats_per_bar", 4), 1, 16);
        offsetSec = std::max (0.0, j.value ("offset_s", 0.0));
        detected = j.value ("detected", false);
        confidence = j.value ("confidence", 0.0);
        state = "ready";
    }
};

struct AppState
{
    std::string folder;                                        // the tracks folder (fixed for the instance)
    std::string exe;                                           // this program (for spawning sessions and agents)
    FirstMixProgress first;                                    // offline first mix (before the page opens)
    std::atomic<im::InteractiveMixerProcessor*> live { nullptr }; // the live engine, once the first mix is in
    std::mutex errMutex;
    std::string error;                                         // first-mix failure, if any
    Exporter exporter;                                         // offline renders of the whole song
    LiveLoudness loudness;                                     // playback at the export loudness
    std::atomic<bool> skipFirstMix { false };                  // the user chose to mix by hand
    std::atomic<bool> analysisWhole { false };                 // agent turns analyze the whole song (else the cycle/excerpt)
    std::atomic<double> lastPagePollMs { 0.0 };                // the page polls status; none for 20 s = stop playback
    AutoAgent autoAgent;                                       // a coding agent started for turns nobody answers
    std::thread tempoThread;                                   // tempo detection (joined at exit)

    mutable std::mutex infoMutex;                              // guards the fields below
    std::string name;                                          // the project's name (default: the folder name)
    TempoState tempo;
    nlohmann::json agentInfo = nlohmann::json::object();       // the coding agent that talked to us: {client, path}

    std::string projectName() const
    {
        std::lock_guard<std::mutex> lk (infoMutex);
        return name;
    }
    std::string phase() const { return live.load() != nullptr ? "live" : first.done ? "starting" : "first_mix"; }
};
} // namespace mfa
