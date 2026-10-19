#include "SongTempo.h"
#include "AppState.h"
#include "PluginProcessor.h"
#include "agent/MixPriors.h"
#include "analysis/Tempo.h"

namespace mfa
{
void startTempoDetection (AppState& app, im::InteractiveMixerProcessor& live)
{
    {
        std::lock_guard<std::mutex> lk (app.infoMutex);
        if (app.tempo.state == "detecting") return;
        app.tempo.state = "detecting";
    }
    // Up to 60 s around the excerpt, from the drums when there are any (their onsets mark the beat best).
    const auto names = live.store.trackNames();
    std::vector<int> drums, all;
    for (int t = 0; t < live.player.numLoadedTracks(); ++t)
    {
        all.push_back (t);
        if (im::priors::roleForName (names[(size_t) t]) == 1) drums.push_back (t);
    }
    const double mid = 0.5 * (live.player.excerptStartSec() + live.player.excerptEndSec());
    const double start = std::max (0.0, mid - 30.0), rate = live.player.loadedSampleRate();
    if (app.tempoThread.joinable()) app.tempoThread.join();
    app.tempoThread = std::thread ([&app, &live, tracks = drums.empty() ? all : drums, start, rate] {
        const auto x = live.player.mixdown (tracks, start, 60.0);
        const auto e = im::estimateTempo (x.data(), (int) x.size(), rate, 4);
        std::lock_guard<std::mutex> lk (app.infoMutex);
        if (app.tempo.state != "detecting") return; // the user set it meanwhile
        if (e.ok)
        {
            app.tempo.bpm = e.bpm;
            app.tempo.beatsPerBar = 4;
            const double bar = 60.0 / e.bpm * 4;
            app.tempo.offsetSec = std::fmod (std::fmod (start + e.offsetSec, bar) + bar, bar); // relative to the song's start
            app.tempo.confidence = e.confidence;
            app.tempo.detected = true;
            app.tempo.state = "ready";
        }
        else
            app.tempo.state = "none";
    });
}
} // namespace mfa
