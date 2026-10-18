// Small native checks of the parts that do not need audio hardware or a coding agent. Run in CI on every platform:
//   mfa-tests   (exit code 0 = all passed)
#include "agent/Bridge.h"
#include "agent/MixPriors.h"
#include "agent/Tools.h"
#include "core/Params.h"
#include "PluginProcessor.h"
#include "Render.h"
#include "standalone/MultitrackPlayer.h"
#include <juce_core/juce_core.h>
#include <iostream>
#include <thread>
#include <functional>
#include <cmath>

namespace
{
int failures = 0;
void check (bool ok, const char* what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
    if (! ok) ++failures;
}

nlohmann::json request()
{
    using nlohmann::json;
    return { { "messages", json::array ({ { { "role", "system" }, { "content", "sys" } }, { { "role", "user" }, { "content", "measurements" } } }) },
             { "tools", json::array ({ { { "type", "function" }, { "function", { { "name", "set_fader" } } } },
                                       { { "type", "function" }, { "function", { { "name", "no_change" } } } } }) } };
}

void bridgeTests()
{
    using nlohmann::json;
    im::Bridge b;
    check (! b.waitTurn (0.05).has_value(), "bridge: no turn before one is asked");

    im::Bridge::Result res;
    std::thread asker ([&] { res = b.ask (request(), "first_mix", "", 10.0); });
    auto t = b.waitTurn (2.0);
    check (t.has_value() && t->trigger == "first_mix", "bridge: the agent gets the waiting turn");
    check (t && t->text.find ("=== SYSTEM ===") != std::string::npos && t->text.find ("set_fader") != std::string::npos,
           "bridge: the turn's text has the system part and the tools");
    auto again = b.waitTurn (0.1);
    check (again && t && again->id == t->id, "bridge: an unanswered turn is offered again");
    check (! b.answer (t->id + 1, { { "content", "x" }, { "tool_calls", json::array() } }).empty(), "bridge: a wrong turn id is refused");
    check (! b.answer (t->id, { { "content", "x" }, { "tool_calls", json::array ({ { { "name", "set_nothing" } } }) } }).empty(),
           "bridge: an unknown tool is refused");
    check (! b.answer (t->id, { { "content", "x" }, { "tool_calls", json::array ({ { { "name", "set_fader" }, { "arguments", 3 } } }) } }).empty(),
           "bridge: arguments must be an object");
    const auto err = b.answer (t->id, { { "content", "louder bass" },
                                        { "tool_calls", json::array ({ { { "name", "set_fader" }, { "arguments", { { "track", "Bass" }, { "db", 2 } } } } }) } });
    check (err.empty(), "bridge: a valid answer is accepted");
    asker.join();
    check (res.ok, "bridge: the asking side gets the answer");
    const auto parsed = im::parseChatResponse (res.response);
    check (parsed.parseError.empty() && parsed.calls.size() == 1 && parsed.calls[0].name == "set_fader" && parsed.content == "louder bass",
           "bridge: the answer parses as a chat completion with the tool call");

    std::thread asker2 ([&] { res = b.ask (request(), "user", "hi", 10.0); });
    b.waitTurn (2.0);
    b.abortPending();
    asker2.join();
    check (! res.ok, "bridge: abortPending releases the waiting turn");
    std::thread asker3 ([&] { res = b.ask (request(), "user", "", 0.2); });
    asker3.join();
    check (! res.ok && res.error.find ("no answer") != std::string::npos, "bridge: a turn times out without an agent");
    b.cancel();
    res = b.ask (request(), "user", "", 10.0);
    check (! res.ok && res.error == "cancelled", "bridge: after cancel, asks return at once");
}

void paramTests()
{
    im::setMaxTracks (150); // more than the old 64-track cap
    check (im::maxTracks() == 150, "params: the track count is set per session");
    check ((int) im::allParams().size() == im::numParams(), "params: the registry has every parameter");
    check (im::findParam ("t150.fader.gain") == im::trackParam (149, im::Fader), "params: the last track's parameters exist");
    check (im::findParam ("master.limiter.ceiling") == im::masterParam (im::LimCeiling), "params: master parameters follow the tracks");
    im::setMaxTracks (8);
    check (im::maxTracks() == 150, "params: the count cannot change once the registry exists");
    const im::TrackMask m { 3 };
    check (m.has (0) && m.has (2) && ! m.has (3) && ! m.has (-1) && m.count() == 3, "params: present tracks are 0..n-1");
}

void roleTests()
{
    using im::priors::roleForName;
    check (roleForName ("Lead Vocal") == 0 && roleForName ("Kick In") == 1 && roleForName ("Bass DI") == 2 && roleForName ("Gtr L") == 3
               && roleForName ("Piano") == 4,
           "groups: guessed from track names");
    check (roleForName ("Harmonica-Amp-CU29") == 4 && roleForName ("Tenor Sax") == 4 && roleForName ("Banjo") == 3
               && roleForName ("Tambourine") == 1 && roleForName ("Cello") == 4,
           "groups: common instruments outside the basic names");
    im::priors::setRoleOverride ("Piano", 0);
    check (roleForName ("Piano") == 0, "groups: a group set by the user wins");
    im::priors::setRoleOverride ("Piano", im::priors::kNoOverride);
    check (roleForName ("Piano") == 4, "groups: clearing it goes back to the name");
}
void noteTests()
{
    using nlohmann::json;
    im::Bridge b;
    std::vector<std::string> notes;
    im::Bridge::Result res;
    std::thread asker ([&] { res = b.ask (request(), "user", "", 10.0, [&] (const std::string& n) { notes.push_back (n); }); });
    auto t = b.waitTurn (2.0);
    check (t && b.pendingFetched(), "notes: a fetched turn counts as being read");
    check (t && ! b.note (t->id + 7, "x").empty(), "notes: a note on another turn is refused");
    check (t && b.note (t->id, "  Guitars own the low-mids.  ").empty() && notes.size() == 1 && notes[0] == "Guitars own the low-mids.",
           "notes: a note reaches the turn's owner, trimmed");
    check (t && ! b.note (t->id, "   ").empty(), "notes: an empty note is refused");
    if (t) b.answer (t->id, { { "content", "ok" }, { "tool_calls", json::array ({ { { "name", "no_change" } } }) } });
    asker.join();
    check (res.ok && ! b.pendingFetched(), "notes: nothing pending after the answer");
    check (! b.note (t ? t->id : 1, "late").empty(), "notes: a note after the answer is refused");
}

// A mono float WAV whose sample i is f(i), in a fresh temp folder.
juce::File writeTestWav (const juce::String& folderName, int frames, double rate, const std::function<float (int)>& f)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (folderName);
    dir.deleteRecursively();
    dir.createDirectory();
    auto file = dir.getChildFile ("Lead Vocal.wav");
    juce::AudioBuffer<float> b (1, frames);
    for (int i = 0; i < frames; ++i) b.setSample (0, i, f (i));
    auto os = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> w (juce::WavAudioFormat().createWriterFor (os.get(), rate, 1, 32, {}, 0));
    os.release();
    w->writeFromAudioSampleBuffer (b, 0, frames);
    return dir;
}

void editTests()
{
    using Player = im::MultitrackPlayer;
    using R = Player::Region;
    { // validation and overlaps
        Player::Regions r { R { 0, 0, 2, 0, 0, 0 }, R { 1, 0, 0.5, 0, 0, 0 } };
        const auto e = Player::normaliseRegions (r, 3.0);
        check (e.empty() && r.size() == 3 && std::abs (r[0].len - 1.0) < 1e-9 && std::abs (r[1].pos - 1.0) < 1e-9 && std::abs (r[2].pos - 1.5) < 1e-9
                   && std::abs (r[2].src - 1.5) < 1e-9 && std::abs (r[2].len - 0.5) < 1e-9,
               "edits: a later clip cuts its span out of an earlier one");
        Player::Regions bad1 { R { 0, 0, 0.005, 0, 0, 0 } }, bad2 { R { 0, 2.5, 1, 0, 0, 0 } }, bad3 { R { 0, 0, 1, 30, 0, 0 } }, bad4 { R { -1, 0, 1, 0, 0, 0 } };
        check (! Player::normaliseRegions (bad1, 3).empty() && ! Player::normaliseRegions (bad2, 3).empty() && ! Player::normaliseRegions (bad3, 3).empty()
                   && ! Player::normaliseRegions (bad4, 3).empty(),
               "edits: too short, past the file, gain out of range and negative positions are refused");
        Player::Regions f { R { 0, 0, 1, 0, 0.8, 0.9 } };
        check (Player::normaliseRegions (f, 3).empty() && f[0].fadeIn <= 0.5 && f[0].fadeOut <= 0.5, "edits: fades are clamped to half the clip");
        Player::Regions out;
        check (! im::InteractiveMixerProcessor::regionsFromJson (nlohmann::json::parse (R"([{"src":0,"len":1}])"), out).empty()
                   && ! im::InteractiveMixerProcessor::regionsFromJson (nlohmann::json::parse (R"([{"pos":"a","src":0,"len":1}])"), out).empty()
                   && im::InteractiveMixerProcessor::regionsFromJson (nlohmann::json::parse (R"([{"pos":1,"src":0,"len":1,"gain":-3}])"), out).empty()
                   && out.size() == 1 && out[0].gainDb == -3,
               "edits: JSON clips need numeric pos, src and len");
    }
    { // rendering: a moved, trimmed, faded, quieter clip
        const double fs = 48000.0;
        const int frames = (int) (3 * fs);
        auto src = [] (int i) { return 0.2f + 0.5f * (float) (i % 4800) / 4800.0f; };
        auto dir = writeTestWav ("mfa-test-edits", frames, fs, src);
        Player p;
        check (p.loadFolder (dir, fs).isEmpty(), "edits: test session loads");
        const float sess = (float) std::pow (10.0, p.inputGainDb() / 20.0);
        // clip: 1 s of the file from 0.5 s, placed at 1.0 s, -6 dB, 0.1 s fades
        check (p.setTrackRegions (0, { R { 1.0, 0.5, 1.0, -6.0, 0.1, 0.1 } }).empty(), "edits: a valid clip is accepted");
        check (std::abs (p.lengthSec() - 3.0) < 1e-3, "edits: the song is at least as long as the longest file");
        p.seek (0.0);
        p.setLooping (false);
        p.setPlaying (true);
        const int total = (int) (2.2 * fs), bs = 512;
        std::vector<float> outL;
        std::vector<float> L (bs), R (bs);
        float* pl[1] = { L.data() };
        float* pr[1] = { R.data() };
        for (int done = 0; done < total; done += bs)
        {
            p.fill (pl, pr, bs);
            outL.insert (outL.end(), L.begin(), L.end());
        }
        auto at = [&] (double sec) { return outL[(size_t) (sec * fs)]; };
        const float g = (float) std::pow (10.0, -6.0 / 20.0);
        const int mid = (int) (1.5 * fs); // timeline 1.5 s = file 1.0 s, past the fade-in
        const float want = src ((int) (1.0 * fs)) * g * sess;
        check (std::abs (at (0.5)) < 1e-7f && std::abs (at (2.1)) < 1e-7f, "edits: silence outside the clip");
        check (std::abs (outL[(size_t) mid] - want) < 1e-4f, "edits: the clip plays the right part of the file at its gain");
        const float half = src ((int) (0.55 * fs)) * g * sess * std::sin (1.5707963f * 0.5f);
        check (std::abs (at (1.05) - half) < 2e-3f, "edits: equal-power fade-in (half way = sin 45 deg)");
        check (std::abs (at (1.999)) < 0.02f * sess, "edits: fade-out reaches silence at the clip end");
        p.setAllEdits ({});
        check (p.editsAreIdentity(), "edits: clearing them goes back to the whole files");
        dir.deleteRecursively();
    }
}

void loudnessTests()
{
    // An offline engine on a generated 4 s session mastered to -14 LUFS, then re-rendered with the gain found.
    const double fs = 48000.0;
    auto dir = writeTestWav ("mfa-test-loud", (int) (4 * fs), fs, [] (int i) { return 0.05f * std::sin (2.0f * 3.14159265f * 220.0f * (float) i / 48000.0f); });
    const bool before = im::gDisableBackgroundServices;
    im::gDisableBackgroundServices = true;
    auto proc = std::make_unique<im::InteractiveMixerProcessor>();
    im::gDisableBackgroundServices = before;
    proc->setRateAndBufferSizeDetails (fs, mfa::kRenderBlock);
    proc->prepareToPlay (fs, mfa::kRenderBlock);
    check (proc->loadSessionFolder (dir).isEmpty(), "loudness: test session loads");
    proc->player.setLooping (false);
    auto r = mfa::masterTo (*proc, fs, -14.0, 0.0, false, {});
    check (r.ok && std::abs (r.lufs + 14.0) < 0.25, ("loudness: converges to the target (" + std::to_string (r.lufs) + " LUFS)").c_str());
    auto again = mfa::masterTo (*proc, fs, -14.0, r.gainDb, true, {});
    check (again.ok && std::abs (again.lufs - r.lufs) < 0.02, "loudness: the same gain renders the same loudness (export = playback)");
    proc.reset();
    dir.deleteRecursively();
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    paramTests();
    bridgeTests();
    roleTests();
    noteTests();
    editTests();
    loudnessTests();
    std::cout << (failures ? "FAILED: " : "all passed") << (failures ? std::to_string (failures) : std::string()) << "\n";
    return failures ? 1 : 0;
}
