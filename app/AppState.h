#pragma once
// What the web server and the MCP endpoints read about this app instance: one session, first mix then live.
#include "Export.h"
#include "FirstMix.h"
#include <atomic>
#include <mutex>
#include <string>

namespace im
{
class InteractiveMixerProcessor;
}

namespace mfa
{
struct AppState
{
    std::string folder;                                        // the tracks folder (fixed for the instance)
    FirstMixProgress first;                                    // offline first mix (before the page opens)
    std::atomic<im::InteractiveMixerProcessor*> live { nullptr }; // the live engine, once the first mix is in
    std::mutex errMutex;
    std::string error;                                         // first-mix failure, if any
    Exporter exporter;                                         // offline renders of the whole song
    std::atomic<bool> skipFirstMix { false };                  // the user chose to mix by hand

    std::string phase() const { return live.load() != nullptr ? "live" : first.done ? "starting" : "first_mix"; }
};
} // namespace mfa
