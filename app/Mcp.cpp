#include "Mcp.h"
#include "AutoAgent.h"
#include "Instances.h"
#include "Recent.h"
#include "core/Paths.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
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
    "TOOLS) -> post_note 2-4 short notes while you work (what you read in the measurements, then your plan; the user "
    "watches them live in the mixer page) -> submit_turn with ALL of the turn's tool calls in one call. The first mix is "
    "5 turns; then the mixer page opens in the user's browser. After that, keep calling next_turn: requests the user "
    "types in the page arrive as new turns. Stop when the user says they are done (close_session ends the console). "
    "The console keeps running when you stop; list_projects shows earlier mixes, and open_session reopens one as it was.";

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
        tool ("post_note",
              "Post a short note on the turn you are working on (one or two sentences: what you read in the measurements, "
              "what you plan to change and why). The user sees it at once in the mixer page while you work, like watching "
              "you think. Post 2-4 per turn, before submit_turn; it does not change the mix.",
              { { "folder", folder },
                { "turn", { { "type", "integer" }, { "description", "The turn id from next_turn." } } },
                { "text", { { "type", "string" }, { "description", "The note (plain text, up to 2000 characters)." } } } },
              json::array ({ "turn", "text" })),
        tool ("session_status", "Where the session is: first mix (turn k of n), or live with the page URL.", { { "folder", folder } }, json::array()),
        tool ("list_projects",
              "The mixes opened lately (newest first): folder, name, when, and the page URL if its console is running. To work "
              "on one again, call open_session with its folder (a saved project reopens as it was).",
              json::object(), json::array()),
        tool ("close_session", "Stop the console for a folder (the page stops working).", { { "folder", folder } }, json::array()),
    });
}

struct Result
{
    std::string text;
    bool error = false;
};

Result err (const std::string& s) { return { s, true }; }

std::string lastOpened; // the folder this MCP process last opened: the default when a call names none
std::string clientName; // the coding agent on the other end (MCP initialize clientInfo.name)
std::set<int> greeted;  // consoles (ports) told which agent this is

httplib::Result call (const Instance& i, const std::string& method, const std::string& path, const json& body = {}, int timeoutS = 10);

// Tells a console which coding agent uses it (and its PATH), once per console: so it can start the same agent for a
// turn later when nobody answers.
void hello (const Instance& i)
{
    if (! greeted.insert (i.port).second) return;
    const char* path = std::getenv ("PATH");
    call (i, "POST", "/api/agent/hello", { { "client", clientName }, { "path", path ? path : "" } }, 3);
}

// The instance for args.folder, else the one this process opened last, else the only open one.
std::optional<Instance> resolve (const json& args, std::string& why)
{
    if (args.contains ("folder") && args["folder"].is_string() && ! args["folder"].get<std::string>().empty())
    {
        auto i = instanceFor (args["folder"].get<std::string>());
        if (! i) why = "no open session for " + args["folder"].get<std::string>() + "; call open_session first";
        else hello (*i);
        return i;
    }
    if (! lastOpened.empty())
        if (auto i = instanceFor (lastOpened))
        {
            hello (*i);
            return i;
        }
    auto all = liveInstances();
    if (all.size() == 1)
    {
        hello (all[0]);
        return all[0];
    }
    why = all.empty() ? "no open session; call open_session(folder) first" : "several sessions are open; pass folder";
    return std::nullopt;
}

httplib::Result call (const Instance& i, const std::string& method, const std::string& path, const json& body, int timeoutS)
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
    std::vector<std::string> a;
    if (args.contains ("turns") && args["turns"].is_number_integer()) a.insert (a.end(), { "--turns", std::to_string (args["turns"].get<int>()) });
    if (args.value ("new_mix", false)) a.push_back ("--new-mix");
    std::string why;
    auto inst = startOrAttach (exe, folder, a, 15.0, why);
    if (! inst) return err (why);
    hello (*inst);
    auto r = call (*inst, "GET", "/api/state");
    if (! r || r->status != 200) return err ("the console does not answer");
    lastOpened = folder;
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
    if (! t.value ("user_msg", std::string()).empty())
        head += (t.value ("trigger", std::string()) == "mix" ? ", the user's goal for this remix: \"" : ", the user asked: \"") + t["user_msg"].get<std::string>() + "\"";
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

Result postNote (const json& args)
{
    std::string why;
    auto inst = resolve (args, why);
    if (! inst) return err (why);
    if (! args.contains ("turn") || ! args["turn"].is_number_integer() || ! args.contains ("text") || ! args["text"].is_string())
        return err ("turn (integer) and text (string) are required");
    auto r = call (*inst, "POST", "/api/agent/note", { { "turn", args["turn"] }, { "text", args["text"] } });
    if (! r) return err ("the console does not answer (closed?)");
    auto j = json::parse (r->body, nullptr, false);
    if (r->status != 200) return err (j.is_discarded() ? r->body : j.value ("error", r->body));
    return { "Noted; the user sees it in the mixer page." };
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

Result listProjects()
{
    const auto recent = loadRecent();
    if (recent.empty()) return { "No mixes yet. open_session(folder) starts one." };
    std::string out = "Mixes opened lately (newest first):\n";
    for (auto& r : recent)
    {
        auto inst = instanceFor (r.folder);
        const auto when = juce::Time ((juce::int64) (r.lastOpened * 1000.0)).formatted ("%Y-%m-%d %H:%M");
        out += "- " + (r.name.empty() ? juce::File (juce::String (r.folder)).getFileName().toStdString() : r.name) + " (" + r.folder + "), "
               + when.toStdString() + (inst ? ", running: " + inst->url : std::string (", not running")) + "\n";
    }
    return { out + "open_session(folder) reopens one as it was saved." };
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
            clientName = params.value ("clientInfo", json::object()).value ("name", std::string());
            const char* path = std::getenv ("PATH");
            AutoAgent::rememberAgent (clientName, path ? path : "");
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
                else if (name == "post_note") r = postNote (args);
                else if (name == "session_status") r = status (args);
                else if (name == "close_session") r = closeSession (args);
                else if (name == "list_projects") r = listProjects();
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
