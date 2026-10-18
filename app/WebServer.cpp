#include "WebServer.h"
#include "AppState.h"
#include "BinaryData.h"
#include "PluginProcessor.h"
#include "SaveDialog.h"
#include "agent/MixPriors.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <limits>

namespace mfa
{
using json = nlohmann::json;
using im::InteractiveMixerProcessor;

namespace
{
const char* kindName (im::Kind k) { return k == im::Kind::Bool ? "bool" : k == im::Kind::Choice ? "choice" : "float"; }

bool loopbackHost (const httplib::Request& r)
{
    const auto h = r.get_header_value ("Host");
    const auto host = h.substr (0, h.rfind (':') == std::string::npos ? h.size() : h.rfind (':'));
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

void reply (httplib::Response& res, const json& j, int status = 200)
{
    res.status = status;
    res.set_content (j.dump(), "application/json");
}

json parseBody (const httplib::Request& req) { return json::parse (req.body, nullptr, false); }
} // namespace

struct WebServer::Impl
{
    AppState& app;
    std::string token;
    httplib::Server http;

    Impl (AppState& a, std::string t) : app (a), token (std::move (t)) {}

    // ---- live engine views ------------------------------------------------------------------------------------
    static int numTracks (const InteractiveMixerProcessor& p) { return p.player.numLoadedTracks(); }

    // Parameters that exist in this session: every slot of the loaded tracks, plus the master section.
    static std::vector<int> liveParams (const InteractiveMixerProcessor& p)
    {
        std::vector<int> v;
        const int n = numTracks (p);
        for (auto& s : im::allParams())
            if (s.track < n) v.push_back (s.index);
        return v;
    }

    static json sessionJson (const InteractiveMixerProcessor& p)
    {
        json tracks = json::array();
        const auto names = p.store.trackNames();
        const auto ov = im::priors::roleOverrides();
        for (int t = 0; t < numTracks (p); ++t)
        {
            const auto& nm = names[(size_t) t];
            tracks.push_back ({ { "index", t }, { "name", nm.empty() ? "Track " + std::to_string (t + 1) : nm },
                                { "group", im::priors::roleForName (nm) }, { "group_set", ov.count (nm) > 0 },
                                { "len", p.player.trackLengthSec (t) } });
        }
        json params = json::array();
        for (int i : liveParams (p))
        {
            const auto& s = im::spec (i);
            json j = { { "i", i }, { "id", s.id }, { "module", s.module }, { "name", s.name }, { "track", s.track },
                       { "kind", kindName (s.kind) }, { "min", s.min }, { "max", s.max }, { "def", s.def },
                       { "step", s.interval }, { "unit", s.unit }, { "minusInf", s.minusInf } };
            if (! s.choices.empty()) j["choices"] = s.choices;
            params.push_back (j);
        }
        return { { "tracks", tracks }, { "params", params }, { "folder", p.player.loadedFolder().getFullPathName().toStdString() },
                 { "rate", p.player.loadedSampleRate() }, { "peaks_block", p.player.peaksBlock() }, { "peaks_ready", p.player.peaksReady() } };
    }

    static json valuesJson (const InteractiveMixerProcessor& p)
    {
        json v = json::object();
        for (int i : liveParams (p)) v[std::to_string (i)] = p.paramValue (i);
        return v;
    }

    // Where the current (or last) agent turn is: listening, waiting (for the coding agent to fetch it), reading (it has
    // the turn), applying, done.
    static json turnJson (InteractiveMixerProcessor& p)
    {
        const auto ts = p.agent.turnStatus();
        if (ts.tick < 0) return nullptr;
        std::string stage = im::Agent::stageName (ts.stage);
        if (ts.stage == im::Agent::Stage::Waiting && im::Bridge::shared().pendingFetched()) stage = "reading";
        return { { "tick", ts.tick }, { "stage", stage }, { "since_s", ts.sinceSec }, { "changes", ts.changes },
                 { "listen_s", p.listenHave.load() }, { "listen_need_s", ts.stage == im::Agent::Stage::Listening ? p.agent.listenSeconds() : 0.0 } };
    }

    json statusJson (InteractiveMixerProcessor& p) const
    {
        json peaks = json::array(), meters = json::array();
        for (int t = 0; t < numTracks (p); ++t)
        {
            const auto& tm = p.engine.meters[(size_t) t];
            peaks.push_back (tm.outPeak.load());
            meters.push_back ({ { "in", tm.inPeak.load() }, { "out", tm.outPeak.load() }, { "out_rms", tm.outRms.load() },
                                { "gr", std::max (tm.compGr.load(), tm.gateGr.load()) } });
        }
        const auto& m = p.engine.masterMeters;
        return { { "playing", p.player.isPlaying() }, { "looping", p.player.isLooping() },
                 { "pos", p.player.positionSec() }, { "len", p.player.lengthSec() },
                 { "loop_region", { p.player.loopStartSec(), p.player.loopEndSec() } },
                 { "excerpt", { p.player.excerptStartSec(), p.player.excerptEndSec() } }, { "input_gain_db", p.player.inputGainDb() },
                 { "master", { m.peakL.load(), m.peakR.load() } }, { "tracks", peaks },
                 { "limGr", m.limGr.load() }, { "busGr", m.busGr.load() }, { "cpu", p.cpuLoad.load() },
                 { "agent", im::Agent::stateName (p.agent.state()) }, { "thinking_s", p.agent.thinkingElapsedSec() },
                 { "agent_seen_s", std::min (im::Bridge::shared().secondsSinceAgentSeen(), 1e6) },
                 { "chat_version", p.store.chatVersion() }, { "song_len", p.player.lengthSec() }, { "meters", meters },
                 { "loudness", app.loudness.status (p) }, { "turn", turnJson (p) }, { "peaks_ready", p.player.peaksReady() },
                 { "edits_version", p.player.editsVersion() } };
    }

    json exportJson() const
    {
        auto& e = app.exporter;
        return { { "running", e.running() }, { "progress", e.progress() }, { "folder", e.folder() }, { "file", e.file() },
                 { "error", e.error() }, { "lufs", e.running() ? 0.0 : e.resultLufs() } };
    }

    static json chatJson (const InteractiveMixerProcessor& p)
    {
        json out = json::array();
        for (auto& e : p.store.chat())
        {
            auto j = e.toJson();
            j.erase ("debugPrompt");
            j.erase ("debugReply");
            out.push_back (j);
        }
        return { { "version", p.store.chatVersion() }, { "entries", out } };
    }

    json stateJson() const
    {
        std::string err;
        {
            std::lock_guard<std::mutex> lk (app.errMutex);
            err = app.error;
        }
        auto& b = im::Bridge::shared();
        return { { "phase", app.phase() }, { "folder", app.folder },
                 { "first_mix", { { "turn", app.first.turn.load() }, { "turns", app.first.turns.load() }, { "done", app.first.done.load() } } },
                 { "error", err }, { "agent_seen_s", std::min (b.secondsSinceAgentSeen(), 1e6) }, { "turn_waiting", b.hasPending() } };
    }

    // ---- routes -----------------------------------------------------------------------------------------------
    // Wraps a handler that needs the live engine (409 while the first mix is still running).
    template <typename F>
    httplib::Server::Handler live (F f)
    {
        return [this, f] (const httplib::Request& req, httplib::Response& res) {
            auto* p = app.live.load();
            if (p == nullptr) return reply (res, { { "error", "the first mix is still running" }, { "state", stateJson() } }, 409);
            f (*p, req, res);
        };
    }

    void routes()
    {
        http.set_pre_routing_handler ([this] (const httplib::Request& req, httplib::Response& res) {
            if (! loopbackHost (req)) { res.status = 403; return httplib::Server::HandlerResponse::Handled; }
            if (req.path.rfind ("/api/", 0) == 0 && req.get_param_value ("t") != token)
            {
                res.status = 401;
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        // Long polls (the agent's turn wait) must not be cut off by the default 5 s read/write timeouts.
        http.set_read_timeout (120, 0);
        http.set_write_timeout (120, 0);

        http.Get ("/", [] (const httplib::Request&, httplib::Response& res) {
            res.set_content (std::string (BinaryData::index_html, (size_t) BinaryData::index_htmlSize), "text/html; charset=utf-8");
            res.set_header ("Cache-Control", "no-store");
        });
        // The page's other files (web/*), by file name; no token (they hold no data).
        http.Get (R"(/([A-Za-z0-9_.\-]+\.(css|js|svg|png|woff2|html)))", [] (const httplib::Request& req, httplib::Response& res) {
            const std::string want = req.matches[1];
            for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
            {
                const char* nm = BinaryData::namedResourceList[i];
                if (want != BinaryData::getNamedResourceOriginalFilename (nm)) continue;
                int size = 0;
                const char* data = BinaryData::getNamedResource (nm, size);
                const std::string ext = req.matches[2];
                const char* type = ext == "css" ? "text/css; charset=utf-8" : ext == "js" ? "text/javascript; charset=utf-8"
                                 : ext == "svg" ? "image/svg+xml" : ext == "png" ? "image/png" : ext == "woff2" ? "font/woff2"
                                                                                                 : "text/html; charset=utf-8";
                res.set_content (std::string (data, (size_t) size), type);
                res.set_header ("Cache-Control", "no-store");
                return;
            }
            res.status = 404;
        });
        http.Get ("/api/state", [this] (const httplib::Request&, httplib::Response& res) { reply (res, stateJson()); });
        // Skip (the rest of) the first mix and open the mixer as it is: for mixing by hand, or when no agent is around.
        http.Post ("/api/skip_first_mix", [this] (const httplib::Request&, httplib::Response& res) {
            app.skipFirstMix = true;
            im::Bridge::shared().abortPending();
            reply (res, { { "ok", true } });
        });

        // Stop the agent's work: the turn waiting for (or with) the coding agent ends with nothing changed, and turns
        // still queued (the rest of a remix) are dropped.
        http.Post ("/api/stop_turns", live ([] (InteractiveMixerProcessor& p, const httplib::Request&, httplib::Response& res) {
            p.agent.cancelQueued();
            im::Bridge::shared().abortPending();
            reply (res, { { "ok", true } });
        }));
        http.Get ("/api/session", live ([] (InteractiveMixerProcessor& p, const httplib::Request&, httplib::Response& res) { reply (res, sessionJson (p)); }));
        http.Get ("/api/values", live ([] (InteractiveMixerProcessor& p, const httplib::Request&, httplib::Response& res) { reply (res, valuesJson (p)); }));
        http.Get ("/api/status", live ([this] (InteractiveMixerProcessor& p, const httplib::Request&, httplib::Response& res) { reply (res, statusJson (p)); }));
        http.Get ("/api/chat", live ([] (InteractiveMixerProcessor& p, const httplib::Request&, httplib::Response& res) { reply (res, chatJson (p)); }));

        // {"i": index, "v": display value}; applied on the message thread with the engine's 20 ms manual ramp.
        http.Post ("/api/param", live ([] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            if (j.is_discarded() || ! j.contains ("i") || ! j.contains ("v")) return reply (res, { { "error", "need i and v" } }, 400);
            const int i = j["i"].get<int>();
            if (i < 0 || i >= im::numParams()) return reply (res, { { "error", "bad index" } }, 400);
            const float v = im::clampToSpec (im::spec (i), j["v"].get<float>());
            juce::MessageManager::callAsync ([&p, i, v] { p.setParamValue (i, v); });
            reply (res, { { "i", i }, { "v", v } });
        }));
        // {"play": bool, "loop": bool, "seek": seconds, "region": [start, end] seconds} (any subset)
        http.Post ("/api/transport", live ([this] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            if (j.is_discarded()) return reply (res, { { "error", "bad json" } }, 400);
            if (j.contains ("play")) p.player.setPlaying (j["play"].get<bool>());
            if (j.contains ("loop")) p.player.setLooping (j["loop"].get<bool>());
            if (j.contains ("seek")) p.player.seek (j["seek"].get<double>());
            if (j.contains ("region") && j["region"].is_array() && j["region"].size() == 2)
                p.player.setLoopRegion (j["region"][0].get<double>(), j["region"][1].get<double>());
            reply (res, statusJson (p));
        }));
        // A prompt from the page: one turn, answered by the coding agent. {"text": "..."}
        http.Post ("/api/prompt", live ([] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            if (j.is_discarded() || ! j.contains ("text") || ! j["text"].is_string()) return reply (res, { { "error", "need text" } }, 400);
            const auto text = j["text"].get<std::string>();
            p.player.setPlaying (true); // the turn measures what plays
            juce::MessageManager::callAsync ([&p, text] { p.agent.userMessage (text); });
            reply (res, { { "ok", true } });
        }));
        // "Mix it": the full multi-turn method on the live engine. {"turns": 5}
        // Remix: the full multi-turn method on the live engine. {"turns": 5, "goal": "..." (optional, the user's aim)}
        http.Post ("/api/mix", live ([] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            const int n = std::clamp (j.is_discarded() ? 5 : j.value ("turns", 5), 1, 10);
            std::string goal = ! j.is_discarded() && j.contains ("goal") && j["goal"].is_string() ? j["goal"].get<std::string>() : std::string();
            goal = juce::String (goal).trim().substring (0, 2000).toStdString();
            p.player.setPlaying (true);
            juce::MessageManager::callAsync ([&p, n, goal] {
                im::ChatEntry e;
                e.role = "user";
                e.text = "Remix (" + std::to_string (n) + (n == 1 ? " turn)" : " turns)") + (goal.empty() ? std::string() : ": " + goal);
                e.time = juce::Time::currentTimeMillis() / 1000.0;
                p.store.addChat (e);
                p.agent.queueTurns (n, "mix", goal);
            });
            reply (res, { { "ok", true }, { "turns", n }, { "goal", goal } });
        }));
        // Render the whole song (and optionally stems) with the current settings and clip edits.
        // {"path": file, "format": "wav"|"flac", "stems": bool, "loudness": lufs | null (as mixed) | absent (= playback)}
        http.Post ("/api/export", live ([this] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            if (j.is_discarded()) j = json::object();
            ExportOptions o;
            o.stems = j.value ("stems", false);
            o.format = j.value ("format", std::string ("wav")) == "flac" ? "flac" : "wav";
            const auto pathStr = j.contains ("path") && j["path"].is_string() ? j["path"].get<std::string>() : std::string();
            o.file = pathStr.empty() ? Exporter::defaultFile (p.player.loadedFolder()) : juce::File (juce::String (pathStr));
            if (! juce::File::isAbsolutePath (o.file.getFullPathName()) || o.file.isDirectory())
                return reply (res, { { "error", "path must be a full file path" } }, 400);
            std::optional<double> l = app.loudness.target();
            if (j.contains ("loudness")) l = j["loudness"].is_number() ? std::optional<double> (j["loudness"].get<double>()) : std::nullopt;
            if (l)
            {
                o.loudnessLufs = std::clamp (*l, -40.0, -5.0);
                o.knownGainDb = app.loudness.currentGain (p, o.loudnessLufs); // the gain playback uses: export = what you hear
            }
            if (app.exporter.running()) return reply (res, { { "error", "an export is already running" } }, 409);
            juce::MessageManager::callAsync ([this, &p, o] { app.exporter.start (p, o); });
            reply (res, { { "ok", true }, { "file", o.file.withFileExtension (o.format == "flac" ? ".flac" : ".wav").getFullPathName().toStdString() },
                          { "reused_playback_gain", o.knownGainDb.has_value() } });
        }));
        http.Get ("/api/export_defaults", live ([] (InteractiveMixerProcessor& p, const httplib::Request&, httplib::Response& res) {
            reply (res, { { "path", Exporter::defaultFile (p.player.loadedFolder()).getFullPathName().toStdString() } });
        }));
        // The OS's own save dialog (blocks this request while it is open). {"path": suggested} -> {path}|{cancelled}|{unavailable}
        http.Post ("/api/choose_save", live ([] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            std::string path = ! j.is_discarded() && j.contains ("path") && j["path"].is_string() ? j["path"].get<std::string>() : std::string();
            if (path.empty() || ! juce::File::isAbsolutePath (juce::String (path))) path = Exporter::defaultFile (p.player.loadedFolder()).getFullPathName().toStdString();
            const auto c = chooseSaveFile (path);
            if (c.kind == SaveChoice::Kind::Chosen) return reply (res, { { "path", c.path } });
            reply (res, c.kind == SaveChoice::Kind::Cancelled ? json { { "cancelled", true } } : json { { "unavailable", true } });
        }));
        http.Post ("/api/reveal", [] (const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            if (j.is_discarded() || ! j.contains ("path") || ! j["path"].is_string()) return reply (res, { { "error", "need path" } }, 400);
            const juce::File f (juce::String (j["path"].get<std::string>()));
            if (! juce::File::isAbsolutePath (f.getFullPathName()) || ! (f.exists() || f.getParentDirectory().isDirectory()))
                return reply (res, { { "error", "no such file" } }, 404);
            revealFile (f.getFullPathName().toStdString());
            reply (res, { { "ok", true } });
        });
        // Playback (and default export) loudness. {"target": -14 | null (as mixed)}
        http.Post ("/api/loudness", live ([this] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            if (j.is_discarded() || ! j.contains ("target")) return reply (res, { { "error", "need target (number or null)" } }, 400);
            std::optional<double> t;
            if (j["target"].is_number()) t = std::clamp (j["target"].get<double>(), -40.0, -5.0);
            else if (! j["target"].is_null()) return reply (res, { { "error", "target must be a number or null" } }, 400);
            juce::MessageManager::callAsync ([this, &p, t] { app.loudness.setTarget (t, &p); });
            reply (res, { { "ok", true }, { "target", t ? json (*t) : json (nullptr) } });
        }));
        // Clip edits. GET -> {version, tracks: [[region...]...]}; POST {"track": t, "regions": [...]} replaces one
        // track's clips (sorted, overlaps cut, later clips win) and returns what is stored.
        http.Get ("/api/edits", live ([] (InteractiveMixerProcessor& p, const httplib::Request&, httplib::Response& res) { reply (res, p.editsJson()); }));
        http.Post ("/api/edits", live ([] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            if (j.is_discarded() || ! j.contains ("track") || ! j["track"].is_number_integer() || ! j.contains ("regions"))
                return reply (res, { { "error", "need track and regions" } }, 400);
            const int t = j["track"].get<int>();
            if (t < 0 || t >= p.player.numLoadedTracks()) return reply (res, { { "error", "bad track" } }, 400);
            im::MultitrackPlayer::Regions r;
            if (auto e = InteractiveMixerProcessor::regionsFromJson (j["regions"], r); ! e.empty()) return reply (res, { { "error", e } }, 400);
            if (auto e = p.player.setTrackRegions (t, std::move (r)); ! e.empty()) return reply (res, { { "error", e } }, 400);
            auto all = p.editsJson();
            reply (res, { { "version", all["version"] }, { "track", t }, { "regions", all["tracks"][(size_t) t] } });
        }));
        // Waveform overview of one track: one byte per peaks_block frames (round(255 * sqrt(peak))).
        http.Get ("/api/peaks", live ([] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            const int t = req.has_param ("track") ? std::atoi (req.get_param_value ("track").c_str()) : -1;
            if (t < 0 || t >= p.player.numLoadedTracks()) return reply (res, { { "error", "bad track" } }, 400);
            if (! p.player.peaksReady()) return reply (res, { { "error", "peaks not ready yet" } }, 503);
            const auto v = p.player.peaks (t);
            res.set_content (std::string (v.begin(), v.end()), "application/octet-stream");
        }));
        http.Get ("/api/export", [this] (const httplib::Request&, httplib::Response& res) { reply (res, exportJson()); });
        // Rename a track and/or set its instrument group. {"index": t, "name": "...", "group": -1..4 | null (= by name)}
        http.Post ("/api/track", live ([] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            if (j.is_discarded() || ! j.contains ("index")) return reply (res, { { "error", "need index" } }, 400);
            const int t = j["index"].get<int>();
            if (t < 0 || t >= p.player.numLoadedTracks()) return reply (res, { { "error", "bad index" } }, 400);
            std::string name;
            if (j.contains ("name") && j["name"].is_string())
            {
                // Tools address tracks by name, so a name must be non-empty and unique (ignoring case).
                name = juce::String (j["name"].get<std::string>()).trim().toStdString();
                if (name.empty()) return reply (res, { { "error", "a track name cannot be empty" } }, 400);
                const auto names = p.store.trackNames();
                for (int k = 0; k < p.player.numLoadedTracks() && k < (int) names.size(); ++k)
                    if (k != t && juce::String (names[(size_t) k]).equalsIgnoreCase (juce::String (name)))
                        return reply (res, { { "error", "another track is already called \"" + names[(size_t) k] + "\"" } }, 409);
            }
            const bool setGroup = j.contains ("group");
            const int group = setGroup && j["group"].is_number_integer() ? j["group"].get<int>() : im::priors::kNoOverride;
            juce::MessageManager::callAsync ([&p, t, name, setGroup, group] {
                const auto old = p.store.trackName (t);
                if (! name.empty() && name != old)
                {
                    const auto ov = im::priors::roleOverrides();
                    if (auto it = ov.find (old); it != ov.end()) // the group follows the track
                    {
                        im::priors::setRoleOverride (old, im::priors::kNoOverride);
                        im::priors::setRoleOverride (name, it->second);
                    }
                    p.store.setTrackName (t, name, im::NameSource::User);
                }
                if (setGroup) im::priors::setRoleOverride (p.store.trackName (t), group);
                p.updateTrackGroups();
            });
            reply (res, { { "ok", true } });
        }));
        http.Post ("/api/undo_turn", live ([] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            if (j.is_discarded() || ! j.contains ("tick")) return reply (res, { { "error", "need tick" } }, 400);
            const int tick = j["tick"].get<int>();
            juce::MessageManager::callAsync ([&p, tick] { p.undoTick (tick); });
            reply (res, { { "ok", true } });
        }));

        // ---- the coding agent's side (used by `mix-final-actual mcp`) ----------------------------------------
        // Waits up to ?wait= seconds (max 50) for a turn. The same turn is returned until it is answered.
        http.Get ("/api/agent/turn", [this] (const httplib::Request& req, httplib::Response& res) {
            const double wait = std::clamp (req.has_param ("wait") ? std::atof (req.get_param_value ("wait").c_str()) : 45.0, 0.0, 50.0);
            auto turn = im::Bridge::shared().waitTurn (wait);
            json j = stateJson();
            j["turn"] = turn ? json { { "id", turn->id }, { "trigger", turn->trigger }, { "user_msg", turn->userMsg }, { "text", turn->text } } : json();
            reply (res, j);
        });
        // {"turn": id, "text": "..."}: a note while the agent works on the turn (shown in the page)
        http.Post ("/api/agent/note", [] (const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            if (j.is_discarded() || ! j.contains ("turn") || ! j["turn"].is_number_integer() || ! j.contains ("text") || ! j["text"].is_string())
                return reply (res, { { "error", "need turn and text" } }, 400);
            const auto err = im::Bridge::shared().note (j["turn"].get<int>(), j["text"].get<std::string>());
            if (! err.empty()) return reply (res, { { "error", err } }, 400);
            reply (res, { { "ok", true } });
        });
        // {"turn": id, "content": "...", "tool_calls": [{"name", "arguments"}]}
        http.Post ("/api/agent/answer", [] (const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            if (j.is_discarded() || ! j.contains ("turn")) return reply (res, { { "error", "need turn, content, tool_calls" } }, 400);
            json r = { { "content", j.value ("content", std::string()) }, { "tool_calls", j.value ("tool_calls", json::array()) } };
            const auto err = im::Bridge::shared().answer (j["turn"].get<int>(), r);
            if (! err.empty()) return reply (res, { { "error", err } }, 400);
            reply (res, { { "ok", true } });
        });
    }
};

WebServer::WebServer (AppState& app, std::string token) : impl (std::make_unique<Impl> (app, std::move (token)))
{
    impl->routes();
}

WebServer::~WebServer() { stop(); }

int WebServer::start (int port)
{
    const int bound = port > 0 ? (impl->http.bind_to_port ("127.0.0.1", port) ? port : -1) : impl->http.bind_to_any_port ("127.0.0.1");
    if (bound <= 0) return -1;
    thread = std::thread ([this] { impl->http.listen_after_bind(); });
    return bound;
}

void WebServer::stop()
{
    impl->http.stop();
    if (thread.joinable()) thread.join();
}
} // namespace mfa
