#include "host/ReaperBridge.h"
#include <pluginterfaces/base/funknown.h>
#include <cstdio>

namespace
{
// From the REAPER SDK (reaper_vst3_interfaces.h).
class IReaperHostApplication : public Steinberg::FUnknown
{
public:
    virtual void* PLUGIN_API getReaperApi (Steinberg::CStringA funcname) = 0;
    virtual void* PLUGIN_API getReaperParent (Steinberg::uint32 w) = 0; // 1 = track, 2 = take, 3 = project
    virtual void* PLUGIN_API reaperExtended (Steinberg::uint32 call, void* p1, void* p2, void* p3) = 0;
};
const Steinberg::TUID kReaperHostIid = INLINE_UID (0x79655E36, 0x77EE4267, 0xA573FEF7, 0x4912C27C);
} // namespace

namespace im
{
bool ReaperBridge::attach (Steinberg::FUnknown* ctx)
{
    api = {};
    if (ctx == nullptr) return false;
    void* obj = nullptr;
    if (ctx->queryInterface (kReaperHostIid, &obj) != Steinberg::kResultOk || obj == nullptr) return false;
    auto* rh = static_cast<IReaperHostApplication*> (obj);
    reaperHost = rh;
    auto get = [rh] (const char* name) { return rh->getReaperApi (name); };
    api.GetTrackNumSends = reinterpret_cast<decltype (api.GetTrackNumSends)> (get ("GetTrackNumSends"));
    api.GetSetTrackSendInfo = reinterpret_cast<decltype (api.GetSetTrackSendInfo)> (get ("GetSetTrackSendInfo"));
    api.GetTrackSendInfo_Value = reinterpret_cast<decltype (api.GetTrackSendInfo_Value)> (get ("GetTrackSendInfo_Value"));
    api.GetTrackName = reinterpret_cast<decltype (api.GetTrackName)> (get ("GetTrackName"));
    api.CountSelectedTracks = reinterpret_cast<decltype (api.CountSelectedTracks)> (get ("CountSelectedTracks"));
    api.GetSelectedTrack = reinterpret_cast<decltype (api.GetSelectedTrack)> (get ("GetSelectedTrack"));
    api.CreateTrackSend = reinterpret_cast<decltype (api.CreateTrackSend)> (get ("CreateTrackSend"));
    api.SetTrackSendInfo_Value = reinterpret_cast<decltype (api.SetTrackSendInfo_Value)> (get ("SetTrackSendInfo_Value"));
    api.SetMediaTrackInfo_Value = reinterpret_cast<decltype (api.SetMediaTrackInfo_Value)> (get ("SetMediaTrackInfo_Value"));
    api.GetMediaTrackInfo_Value = reinterpret_cast<decltype (api.GetMediaTrackInfo_Value)> (get ("GetMediaTrackInfo_Value"));
    api.Undo_BeginBlock2 = reinterpret_cast<decltype (api.Undo_BeginBlock2)> (get ("Undo_BeginBlock2"));
    api.Undo_EndBlock2 = reinterpret_cast<decltype (api.Undo_EndBlock2)> (get ("Undo_EndBlock2"));
    api.parentTrack = rh->getReaperParent (1);
    api.ok = api.GetTrackNumSends && api.GetSetTrackSendInfo && api.GetTrackSendInfo_Value && api.GetTrackName;
    rh->release(); // queryInterface added a reference; the host context outlives the plugin
    return api.ok;
}

std::array<std::string, kMaxTracks> ReaperBridge::inputNames() const
{
    std::array<std::string, kMaxTracks> out {};
    if (! api.ok) return out;
    // parent track can change if the FX is moved; ask each time
    void* track = reaperHost ? static_cast<IReaperHostApplication*> (reaperHost)->getReaperParent (1) : api.parentTrack;
    if (track == nullptr) return out;
    char buf[512];
    const int n = api.GetTrackNumSends (track, -1); // category < 0: receives
    for (int i = 0; i < n; ++i)
    {
        if (api.GetTrackSendInfo_Value (track, -1, i, "B_MUTE") > 0.5) continue;
        const int dst = (int) api.GetTrackSendInfo_Value (track, -1, i, "I_DSTCHAN");
        const int pair = (dst & 1023) / 2;
        if (pair < 0 || pair >= kMaxTracks || ! out[(size_t) pair].empty()) continue;
        void* src = api.GetSetTrackSendInfo (track, -1, i, "P_SRCTRACK", nullptr);
        if (src != nullptr && api.GetTrackName (src, buf, (int) sizeof (buf))) out[(size_t) pair] = buf;
    }
    if (out[0].empty() && api.GetTrackName (track, buf, (int) sizeof (buf))) out[0] = buf;
    return out;
}

std::string ReaperBridge::routeSelectedTracks()
{
    if (! api.ok || ! api.CountSelectedTracks || ! api.GetSelectedTrack || ! api.CreateTrackSend || ! api.SetTrackSendInfo_Value
        || ! api.SetMediaTrackInfo_Value || ! api.GetMediaTrackInfo_Value)
        return "REAPER routing API not available";
    void* me = reaperHost ? static_cast<IReaperHostApplication*> (reaperHost)->getReaperParent (1) : api.parentTrack;
    if (me == nullptr) return "could not find this plugin's track";
    // pairs already used by existing receives
    std::array<bool, kMaxTracks> used {};
    const int nRecv = api.GetTrackNumSends (me, -1);
    for (int i = 0; i < nRecv; ++i)
    {
        const int pair = ((int) api.GetTrackSendInfo_Value (me, -1, i, "I_DSTCHAN") & 1023) / 2;
        if (pair >= 0 && pair < kMaxTracks) used[(size_t) pair] = true;
    }
    if (api.Undo_BeginBlock2) api.Undo_BeginBlock2 (nullptr);
    if (api.GetMediaTrackInfo_Value (me, "I_NCHAN") < 2 * kMaxTracks) api.SetMediaTrackInfo_Value (me, "I_NCHAN", 2 * kMaxTracks);
    int routed = 0, skipped = 0;
    const int nSel = api.CountSelectedTracks (nullptr);
    for (int i = 0; i < nSel; ++i)
    {
        void* src = api.GetSelectedTrack (nullptr, i);
        if (src == nullptr || src == me) continue;
        int pair = -1;
        for (int k = 0; k < kMaxTracks; ++k)
            if (! used[(size_t) k]) { pair = k; break; }
        if (pair < 0) { ++skipped; continue; }
        const int sendIdx = api.CreateTrackSend (src, me);
        if (sendIdx < 0) { ++skipped; continue; }
        api.SetTrackSendInfo_Value (src, 0, sendIdx, "I_SRCCHAN", 0);           // stereo 1/2
        api.SetTrackSendInfo_Value (src, 0, sendIdx, "I_DSTCHAN", 2.0 * pair);  // -> our pair
        api.SetMediaTrackInfo_Value (src, "B_MAINSEND", 0);                       // only via the mixer
        used[(size_t) pair] = true;
        ++routed;
    }
    if (api.Undo_EndBlock2) api.Undo_EndBlock2 (nullptr, "Interactive Mixer: route selected tracks", -1);
    std::string r = "routed " + std::to_string (routed) + " track" + (routed == 1 ? "" : "s") + " into the mixer";
    if (skipped > 0) r += " (" + std::to_string (skipped) + " skipped: all " + std::to_string (kMaxTracks) + " inputs in use)";
    if (nSel == 0) r = "select the source tracks in REAPER first (not the mixer's own track)";
    return r;
}

std::string ReaperBridge::diagnostics() const
{
    if (! api.ok) return "REAPER API: not available (not REAPER, or not the VST3 build)";
    const auto names = inputNames();
    std::string s = "REAPER API: ok; inputs:";
    for (int t = 0; t < kMaxTracks; ++t) s += " [" + std::to_string (t + 1) + "] " + (names[(size_t) t].empty() ? "-" : names[(size_t) t]);
    return s;
}
} // namespace im
