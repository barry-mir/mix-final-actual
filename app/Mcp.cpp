#include "Mcp.h"
#include "Instances.h"
#include "core/Paths.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <csignal>
#endif

namespace mfa
{
using json = nlohmann::json;

namespace
{
const char* kVersion = "0.1.0";

const char* kInstructions =
    "Mixes a folder of audio tracks on a real mixing console. You never hear audio: each turn the console measures the "
    "mix and sends you a text report and its console tools; you answer with tool calls. Workflow: open_session(folder) "
    "starts the console and its first mix; then loop: next_turn -> read the turn (follow its SYSTEM part, use only its "
    "TOOLS) -> submit_turn with ALL of the turn's tool calls in one call. The first mix is 5 turns; then the mixer page "
    "opens in the user's browser. After that, keep calling next_turn: prompts the user types in the page arrive as new "
    "turns. Stop when the user says they are done (close_session ends the console).";

json tool (const char* name, const char* desc, json props, json required)
{
    return { { "name", name }, { "description", desc },
             { "inputSchema", { { "type", "object" }, { "properties", std::move (props) }, { "required", std::move (required) } } } };
}

json toolList()
{
    const json folder = { { "type", "string" }, { "description", "Absolute path of the tracks folder (one audio file per track, named by instrument). Optional when only one session is open." } };
    return json::array ({
        tool ("open_session",
              "Start the console on a folder of tracks (WAV/AIFF/FLAC, one file per track, file names = track names such as "
              "'Lead Vocal.wav'). Starts the first mix, which waits for your turns; a folder with a saved project reopens it "
              "instead (live at once). Reuses the session if it is already open.",
              { { "folder", { { "type", "string" }, { "description", "Absolute path of the tracks folder." } } },
                { "turns", { { "type", "integer" }, { "description", "Turns in the first mix (default 5)." } } },
                { "new_mix", { { "type", "boolean" }, { "description", "Start over with a new first mix even if the folder has a saved project (default false: a saved project is reopened as it was)." } } } },
              json::array ({ "folder" })),
        tool ("next_turn",
              "Wait for the console's next turn and return it: a SYSTEM part (how to mix, how to answer), a USER part (this "
              "turn's measurements, your recent turns and their effect, the user's request if any) and the TOOLS (function "
              "schemas). Returns 'no turn' if none arrives within wait_seconds; call it again to keep waiting.",
              { { "folder", folder }, { "wait_seconds", { { "type", "number" }, { "description", "Up to 50 (default 45)." } } } },
              json::array()),
        tool ("submit_turn",
              "Answer a turn: content = the reply text the turn's SYSTEM part asks for; tool_calls = every console tool call of "
              "this turn, in order, each {name, arguments} following the turn's TOOLS schemas. One call per turn.",
              { { "folder", folder },
                { "turn", { { "type", "integer" }, { "description", "The turn id from next_turn." } } },
                { "content", { { "type", "string" } } },
                { "tool_calls", { { "type", "array" },
                                  { "items", { { "type", "object" },
                                               { "properties", { { "name", { { "type", "string" } } }, { "arguments", { { "type", "object" } } } } },
                                               { "required", json::array ({ "name" }) } } } } } },
              json::array ({ "turn", "content", "tool_calls" })),
        tool ("session_status", "Where the session is: first mix (turn k of n), or live with the page URL.", { { "folder", folder } }, json::array()),
        tool ("close_session", "Stop the console for a folder (the page stops working).", { { "folder", folder } }, json::array()),
    });
}

struct Result
{
    std::string text;
    bool error = false;
};

Result err (const std::string& s) { return { s, true }; }

// The instance for args.folder, or the only open one.
std::optional<Instance> resolve (const json& args, std::string& why)
{
    if (args.contains ("folder") && args["folder"].is_string() && ! args["folder"].get<std::string>().empty())
    {
        auto i = instanceFor (args["folder"].get<std::string>());
        if (! i) why = "no open session for " + args["folder"].get<std::string>() + "; call open_session first";
        return i;
    }
    auto all = liveInstances();
    if (all.size() == 1) return all[0];
    why = all.empty() ? "no open session; call open_session(folder) first" : "several sessions are open; pass folder";
    return std::nullopt;
}

httplib::Result call (const Instance& i, const std::string& method, const std::string& path, const json& body = {}, int timeoutS = 10)
{
    httplib::Client c ("127.0.0.1", i.port);
    c.set_connection_timeout (3, 0);
    c.set_read_timeout (timeoutS, 0);
    const auto p = path + (path.find ('?') == std::string::npos ? "?" : "&") + "t=" + i.token;
    if (method == "POST") return c.Post (p, body.dump(), "application/json");
    return c.Get (p);
}

std::string describeState (const json& s, const Instance& i)
{
    const auto phase = s.value ("phase", std::string());
    if (phase == "first_mix")
    {
        const auto& f = s["first_mix"];
        return "First mix in progress: turn " + std::to_string (f.value ("turn", 0)) + " of " + std::to_string (f.value ("turns", 5))
               + ". Call next_turn to get the waiting turn.";
    }
    std::string out = "Live: the mixer page is open for the user at " + i.url
                      + " . Prompts the user types there arrive as turns; call next_turn to wait for them.";
    if (! s.value ("error", std::string()).empty()) out += " (first mix: " + s["error"].get<std::string>() + ")";
    return out;
}

Result openSession (const json& args, const std::string& exe)
{
    if (! args.contains ("folder") || ! args["folder"].is_string()) return err ("folder is required");
    const auto folder = normaliseFolder (args["folder"].get<std::string>());
    if (! juce::File (juce::String (folder)).isDirectory()) return err ("not a folder: " + folder);
    auto inst = instanceFor (folder);
    if (! inst)
    {
        std::vector<std::string> a { "serve", folder };
        if (args.contains ("turns") && args["turns"].is_number_integer()) a.insert (a.end(), { "--turns", std::to_string (args["turns"].get<int>()) });
        if (args.value ("new_mix", false)) a.push_back ("--new-mix");
        const auto log = im::appDataDir().getChildFile ("logs");
        log.createDirectory();
        if (! spawnDetached (exe, a, log.getChildFile ("serve.log").getFullPathName().toStdString()))
            return err ("could not start the console (" + exe + ")");
        for (int k = 0; k < 150 && ! inst; ++k) // up to 15 s for the instance file
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
            inst = instanceFor (folder);
        }
        if (! inst) return err ("the console did not start; see " + log.getChildFile ("serve.log").getFullPathName().toStdString());
    }
    auto r = call (*inst, "GET", "/api/state");
    if (! r || r->status != 200) return err ("the console does not answer");
    return { "Session open for " + folder + ". " + describeState (json::parse (r->body), *inst) };
}

Result nextTurn (const json& args)
{
    std::string why;
    auto inst = resolve (args, why);
    if (! inst) return err (why);
    const double wait = std::clamp (args.value ("wait_seconds", 45.0), 0.0, 50.0);
    auto r = call (*inst, "GET", "/api/agent/turn?wait=" + std::to_string ((int) wait), {}, (int) wait + 15);
    if (! r || r->status != 200) return err ("the console does not answer (closed?)");
    auto j = json::parse (r->body, nullptr, false);
    if (j.is_discarded()) return err ("bad reply from the console");
    if (j["turn"].is_null()) return { "No turn waiting. " + describeState (j, *inst) };
    const auto& t = j["turn"];
    const int id = t.value ("id", 0);
    std::string head = "TURN " + std::to_string (id) + " (" + t.value ("trigger", std::string());
    if (j.value ("phase", std::string()) == "first_mix")
        head += ", first mix " + std::to_string (j["first_mix"].value ("turn", 0)) + "/" + std::to_string (j["first_mix"].value ("turns", 5));
    if (! t.value ("user_msg", std::string()).empty()) head += ", the user asked: \"" + t["user_msg"].get<std::string>() + "\"";
    head += "). Answer with submit_turn(turn=" + std::to_string (id) + ", content, tool_calls).\n\n";
    const auto text = t.value ("text", std::string());
    // Also on disk, for agents that hand turns to a helper that reads files.
    auto dir = im::appDataDir().getChildFile ("turns");
    dir.createDirectory();
    auto f = dir.getChildFile ("turn_" + juce::String (id) + ".txt");
    f.replaceWithText (text);
    if (text.size() > 100000)
        return { head + "The turn is " + std::to_string (text.size()) + " characters; read it from " + f.getFullPathName().toStdString() + " (in chunks)." };
    return { head + text + "\n(also saved at " + f.getFullPathName().toStdString() + ")" };
}

Result submitTurn (const json& args)
{
    std::string why;
    auto inst = resolve (args, why);
    if (! inst) return err (why);
    if (! args.contains ("turn") || ! args["turn"].is_number_integer()) return err ("turn (integer) is required");
    json body = { { "turn", args["turn"] }, { "content", args.value ("content", std::string()) }, { "tool_calls", args.value ("tool_calls", json::array()) } };
    auto r = call (*inst, "POST", "/api/agent/answer", body);
    if (! r) return err ("the console does not answer (closed?)");
    auto j = json::parse (r->body, nullptr, false);
    if (r->status != 200) return err (j.is_discarded() ? r->body : j.value ("error", r->body));
    return { "Turn " + std::to_string (args["turn"].get<int>()) + " accepted (" + std::to_string (body["tool_calls"].size())
             + " tool calls). The console applies it and measures again; call next_turn for the next turn." };
}

Result status (const json& args)
{
    std::string why;
    auto inst = resolve (args, why);
    if (! inst) return err (why);
    auto r = call (*inst, "GET", "/api/state");
    if (! r || r->status != 200) return err ("the console does not answer");
    return { describeState (json::parse (r->body), *inst) };
}

Result closeSession (const json& args)
{
    std::string why;
    auto inst = resolve (args, why);
    if (! inst) return err (why);
#if JUCE_WINDOWS
    HANDLE h = OpenProcess (PROCESS_TERMINATE, FALSE, (DWORD) inst->pid);
    if (h) { TerminateProcess (h, 0); CloseHandle (h); }
#else
    ::kill (inst->pid, SIGTERM);
#endif
    return { "Closed the session for " + inst->folder + "." };
}

void send (const json& msg)
{
    std::cout << msg.dump() << "\n";
    std::cout.flush();
}
} // namespace

int runMcp (const char* argv0)
{
    auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    if (exe.empty()) exe = argv0;
    std::ios::sync_with_stdio (false);
    std::string line;
    while (std::getline (std::cin, line))
    {
        if (line.empty()) continue;
        auto msg = json::parse (line, nullptr, false);
        if (msg.is_discarded() || ! msg.is_object())
        {
            send ({ { "jsonrpc", "2.0" }, { "id", nullptr }, { "error", { { "code", -32700 }, { "message", "parse error" } } } });
            continue;
        }
        const auto method = msg.value ("method", std::string());
        if (! msg.contains ("id")) continue; // notifications (initialized, cancelled) need no reply
        const auto id = msg["id"];
        const auto params = msg.value ("params", json::object());
        auto ok = [&] (json result) { send ({ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move (result) } }); };
        if (method == "initialize")
        {
            const auto want = params.value ("protocolVersion", std::string ("2025-06-18"));
            ok ({ { "protocolVersion", want }, { "capabilities", { { "tools", json::object() } } },
                  { "serverInfo", { { "name", "mix-final-actual" }, { "version", kVersion } } }, { "instructions", kInstructions } });
        }
        else if (method == "ping")
            ok (json::object());
        else if (method == "tools/list")
            ok ({ { "tools", toolList() } });
        else if (method == "tools/call")
        {
            const auto name = params.value ("name", std::string());
            const auto args = params.value ("arguments", json::object());
            Result r;
            try
            {
                if (name == "open_session") r = openSession (args, exe);
                else if (name == "next_turn") r = nextTurn (args);
                else if (name == "submit_turn") r = submitTurn (args);
                else if (name == "session_status") r = status (args);
                else if (name == "close_session") r = closeSession (args);
                else r = err ("unknown tool " + name);
            }
            catch (const std::exception& e)
            {
                r = err (std::string ("error: ") + e.what());
            }
            ok ({ { "content", json::array ({ { { "type", "text" }, { "text", r.text } } }) }, { "isError", r.error } });
        }
        else
            send ({ { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", -32601 }, { "message", "method not found: " + method } } } });
    }
    return 0;
}
} // namespace mfa
