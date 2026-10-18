#include "WebServer.h"
#include "BinaryData.h"
#include "PluginProcessor.h"
#include <httplib.h>
#include <nlohmann/json.hpp>

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
} // namespace

struct WebServer::Impl
{
    InteractiveMixerProcessor& p;
    std::string token;
    httplib::Server http;

    Impl (InteractiveMixerProcessor& proc, std::string t) : p (proc), token (std::move (t)) {}

    int numTracks() const { return p.player.numLoadedTracks(); }

    // Parameters that exist in this session: every slot of the loaded tracks, plus the master section.
    std::vector<int> liveParams() const
    {
        std::vector<int> v;
        const int n = numTracks();
        for (auto& s : im::allParams())
            if (s.track < n) v.push_back (s.index);
        return v;
    }

    json sessionJson() const
    {
        json tracks = json::array();
        const auto names = p.store.trackNames();
        for (int t = 0; t < numTracks(); ++t)
            tracks.push_back ({ { "index", t }, { "name", t < (int) names.size() ? names[(size_t) t] : "Track " + std::to_string (t + 1) } });
        json params = json::array();
        for (int i : liveParams())
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

    json valuesJson() const
    {
        json v = json::object();
        for (int i : liveParams()) v[std::to_string (i)] = p.paramValue (i);
        return v;
    }

    json statusJson() const
    {
        json peaks = json::array();
        for (int t = 0; t < numTracks(); ++t) peaks.push_back (p.engine.meters[(size_t) t].outPeak.load());
        const auto& m = p.engine.masterMeters;
        return { { "playing", p.player.isPlaying() }, { "looping", p.player.isLooping() },
                 { "pos", p.player.positionSec() }, { "len", p.player.lengthSec() },
                 { "master", { m.peakL.load(), m.peakR.load() } }, { "tracks", peaks },
                 { "limGr", m.limGr.load() }, { "busGr", m.busGr.load() }, { "cpu", p.cpuLoad.load() } };
    }

    static void reply (httplib::Response& res, const json& j) { res.set_content (j.dump(), "application/json"); }

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
        http.Get ("/", [] (const httplib::Request&, httplib::Response& res) {
            res.set_content (std::string (BinaryData::index_html, (size_t) BinaryData::index_htmlSize), "text/html; charset=utf-8");
        });
        http.Get ("/api/session", [this] (const httplib::Request&, httplib::Response& res) { reply (res, sessionJson()); });
        http.Get ("/api/values", [this] (const httplib::Request&, httplib::Response& res) { reply (res, valuesJson()); });
        http.Get ("/api/status", [this] (const httplib::Request&, httplib::Response& res) { reply (res, statusJson()); });
        // {"i": index, "v": display value}; applied on the message thread with the engine's 20 ms manual ramp.
        http.Post ("/api/param", [this] (const httplib::Request& req, httplib::Response& res) {
            auto j = json::parse (req.body, nullptr, false);
            if (j.is_discarded() || ! j.contains ("i") || ! j.contains ("v")) { res.status = 400; return; }
            const int i = j["i"].get<int>();
            if (i < 0 || i >= im::kNumParams) { res.status = 400; return; }
            const float v = im::clampToSpec (im::spec (i), j["v"].get<float>());
            juce::MessageManager::callAsync ([this, i, v] { p.setParamValue (i, v); });
            reply (res, { { "i", i }, { "v", v } });
        });
        // {"play": bool, "loop": bool, "seek": seconds} (any subset)
        http.Post ("/api/transport", [this] (const httplib::Request& req, httplib::Response& res) {
            auto j = json::parse (req.body, nullptr, false);
            if (j.is_discarded()) { res.status = 400; return; }
            if (j.contains ("play")) p.player.setPlaying (j["play"].get<bool>());
            if (j.contains ("loop")) p.player.setLooping (j["loop"].get<bool>());
            if (j.contains ("seek")) p.player.seek (j["seek"].get<double>());
            reply (res, statusJson());
        });
    }
};

WebServer::WebServer (InteractiveMixerProcessor& proc, std::string token) : impl (std::make_unique<Impl> (proc, std::move (token)))
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
