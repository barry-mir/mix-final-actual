#include "WebServer.h"
#include "AppState.h"
#include "BinaryData.h"
#include "PluginProcessor.h"
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
                                { "group", im::priors::roleForName (nm) }, { "group_set", ov.count (nm) > 0 } });
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
        return { { "tracks", tracks }, { "params", params }, { "folder", p.player.loadedFolder().getFullPathName().toStdString() } };
    }

    static json valuesJson (const InteractiveMixerProcessor& p)
    {
        json v = json::object();
        for (int i : liveParams (p)) v[std::to_string (i)] = p.paramValue (i);
        return v;
    }

    static json statusJson (const InteractiveMixerProcessor& p)
    {
        json peaks = json::array();
        for (int t = 0; t < numTracks (p); ++t) peaks.push_back (p.engine.meters[(size_t) t].outPeak.load());
        const auto& m = p.engine.masterMeters;
        return { { "playing", p.player.isPlaying() }, { "looping", p.player.isLooping() },
                 { "pos", p.player.positionSec() }, { "len", p.player.lengthSec() },
                 { "loop_region", { p.player.loopStartSec(), p.player.loopEndSec() } },
                 { "excerpt", { p.player.excerptStartSec(), p.player.excerptEndSec() } }, { "input_gain_db", p.player.inputGainDb() },
                 { "master", { m.peakL.load(), m.peakR.load() } }, { "tracks", peaks },
                 { "limGr", m.limGr.load() }, { "busGr", m.busGr.load() }, { "cpu", p.cpuLoad.load() },
                 { "agent", im::Agent::stateName (p.agent.state()) }, { "thinking_s", p.agent.thinkingElapsedSec() },
                 { "chat_version", p.store.chatVersion() } };
    }

    json exportJson() const
    {
        auto& e = app.exporter;
        return { { "running", e.running() }, { "progress", e.progress() }, { "folder", e.folder() }, { "error", e.error() },
                 { "lufs", e.running() ? 0.0 : e.resultLufs() } };
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
        });
        http.Get ("/api/state", [this] (const httplib::Request&, httplib::Response& res) { reply (res, stateJson()); });
        // Skip (the rest of) the first mix and open the mixer as it is: for mixing by hand, or when no agent is around.
        http.Post ("/api/skip_first_mix", [this] (const httplib::Request&, httplib::Response& res) {
            app.skipFirstMix = true;
            im::Bridge::shared().abortPending();
            reply (res, { { "ok", true } });
        });

        http.Get ("/api/session", live ([] (InteractiveMixerProcessor& p, const httplib::Request&, httplib::Response& res) { reply (res, sessionJson (p)); }));
        http.Get ("/api/values", live ([] (InteractiveMixerProcessor& p, const httplib::Request&, httplib::Response& res) { reply (res, valuesJson (p)); }));
        http.Get ("/api/status", live ([] (InteractiveMixerProcessor& p, const httplib::Request&, httplib::Response& res) { reply (res, statusJson (p)); }));
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
        http.Post ("/api/transport", live ([] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
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
        http.Post ("/api/mix", live ([] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            const int n = std::clamp (j.is_discarded() ? 5 : j.value ("turns", 5), 1, 10);
            p.player.setPlaying (true);
            p.agent.queueTurns (n, "mix");
            reply (res, { { "ok", true }, { "turns", n } });
        }));
        // Render the whole song (and optionally stems) with the current settings. {"stems": bool}
        http.Post ("/api/export", live ([this] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            const bool stems = ! j.is_discarded() && j.value ("stems", false);
            const double lufs = ! j.is_discarded() && j.contains ("loudness") && j["loudness"].is_number() ? j["loudness"].get<double>()
                                                                                                        : std::numeric_limits<double>::quiet_NaN();
            if (app.exporter.running()) return reply (res, { { "error", "an export is already running" } }, 409);
            juce::MessageManager::callAsync ([this, &p, stems, lufs] { app.exporter.start (p, stems, lufs); });
            reply (res, { { "ok", true } });
        }));
        http.Get ("/api/export", [this] (const httplib::Request&, httplib::Response& res) { reply (res, exportJson()); });
        // Rename a track and/or set its instrument group. {"index": t, "name": "...", "group": -1..4 | null (= by name)}
        http.Post ("/api/track", live ([] (InteractiveMixerProcessor& p, const httplib::Request& req, httplib::Response& res) {
            auto j = parseBody (req);
            if (j.is_discarded() || ! j.contains ("index")) return reply (res, { { "error", "need index" } }, 400);
            const int t = j["index"].get<int>();
            if (t < 0 || t >= p.player.numLoadedTracks()) return reply (res, { { "error", "bad index" } }, 400);
            std::string name = j.contains ("name") && j["name"].is_string() ? j["name"].get<std::string>() : std::string();
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
