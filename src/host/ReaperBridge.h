#pragma once
// Track names from the host.
//
// Hosts do not pass the names of tracks feeding a plugin's inputs. REAPER, however, exposes its
// extension API to VST3 plugins (IReaperHostApplication, REAPER 5.02+). With it we find our own
// track and its receives, and map each receive's destination channel pair to an input pair:
//   receive "Vocal" -> channels 3/4 of our track  =>  input 2 is named "Vocal".
// Our own track's name is used for input 1 if no receive lands on channels 1/2.
// Call only on the message thread (REAPER API rule).
#include "core/Params.h"
#include <array>
#include <string>

namespace Steinberg { class FUnknown; }

namespace im
{
class ReaperBridge
{
public:
    bool attach (Steinberg::FUnknown* hostContext); // true if the host is REAPER
    bool available() const noexcept { return api.ok; }

    // Names per input pair; empty string = unknown.
    std::array<std::string, kMaxTracks> inputNames() const;
    std::string diagnostics() const; // for the host log

    // Create sends from the selected tracks to this plugin's track, one stereo pair each
    // (1/2, 3/4, ...), set the plugin track to 2*kMaxTracks channels; one undo step.
    // Returns a human-readable result.
    std::string routeSelectedTracks();

private:
    struct Api
    {
        bool ok = false;
        void* parentTrack = nullptr;
        int (*GetTrackNumSends) (void* tr, int category) = nullptr;
        void* (*GetSetTrackSendInfo) (void* tr, int category, int idx, const char* parm, void* setNew) = nullptr;
        double (*GetTrackSendInfo_Value) (void* tr, int category, int idx, const char* parm) = nullptr;
        bool (*GetTrackName) (void* tr, char* buf, int sz) = nullptr;
        int (*CountSelectedTracks) (void* proj) = nullptr;
        void* (*GetSelectedTrack) (void* proj, int idx) = nullptr;
        int (*CreateTrackSend) (void* tr, void* dest) = nullptr;
        bool (*SetTrackSendInfo_Value) (void* tr, int category, int idx, const char* parm, double v) = nullptr;
        bool (*SetMediaTrackInfo_Value) (void* tr, const char* parm, double v) = nullptr;
        double (*GetMediaTrackInfo_Value) (void* tr, const char* parm) = nullptr;
        void (*Undo_BeginBlock2) (void* proj) = nullptr;
        void (*Undo_EndBlock2) (void* proj, const char* desc, int flags) = nullptr;
    } api;
    void* reaperHost = nullptr; // IReaperHostApplication*
};
} // namespace im
