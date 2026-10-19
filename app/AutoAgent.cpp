#include "AutoAgent.h"
#include "PluginProcessor.h"
#include "core/Paths.h"
#include <cstring>
#include <set>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <csignal>
 #include <fcntl.h>
 #include <spawn.h>
 #include <sys/wait.h>
 #include <unistd.h>
extern char** environ;
#endif

namespace mfa
{
using json = nlohmann::json;

namespace
{
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
juce::File agentFile() { return im::appDataDir().getChildFile ("agent.json"); }
const char* kServer = "mix-final-actual";
const char* kTools[] = { "open_session", "next_turn", "submit_turn", "post_note", "session_status" };

std::string kindOf (const std::string& client)
{
    const auto c = juce::String (client).toLowerCase();
    if (c.contains ("claude")) return "claude";
    if (c.contains ("codex")) return "codex";
    return {};
}

std::string prompt (const std::string& folder)
{
    return "Answer the mixing console's waiting turns for the tracks in '" + folder + "'. Use only the " + kServer
           + " MCP tools and do nothing else (no files, no shell, no other tools). 1. Call open_session with folder '" + folder
           + "' (the console is already running; this attaches to it). 2. Call next_turn. When it returns a turn, read all of it "
             "and follow its SYSTEM part exactly; post 1 to 3 short notes on the turn with post_note (first what you read in the "
             "measurements, then your plan); then answer with ONE submit_turn(turn, content, tool_calls) holding all of the turn's "
             "tool calls, following its TOOLS schemas (if submit_turn reports an error, fix the answer and submit the same turn "
             "again). Then call next_turn again. 3. When next_turn says no turn is waiting two times in a row, stop and reply with "
             "one short sentence.";
}

std::vector<std::string> searchDirs (const std::string& agentPath)
{
    std::vector<std::string> dirs;
    const auto sep = juce::File::getSeparatorChar() == '\\' ? ';' : ':';
    for (const auto& p : { agentPath, std::string (std::getenv ("PATH") ? std::getenv ("PATH") : "") })
        for (auto& d : juce::StringArray::fromTokens (juce::String (p), juce::String::charToString (sep), ""))
            if (d.isNotEmpty()) dirs.push_back (d.toStdString());
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    for (auto& d : { home.getChildFile (".local/bin"), juce::File ("/opt/homebrew/bin"), juce::File ("/usr/local/bin"),
                     home.getChildFile (".npm-global/bin"), home.getChildFile (".claude/local"), home.getChildFile (".bun/bin"),
                     juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("npm") })
        dirs.push_back (d.getFullPathName().toStdString());
    return dirs;
}

std::string joinPath (const std::vector<std::string>& dirs)
{
    std::string out;
    const char sep = juce::File::getSeparatorChar() == '\\' ? ';' : ':';
    std::set<std::string> seen; // exact matches only: "/usr/bin" is not the same folder as ".../bootstrap/usr/bin"
    for (auto& d : dirs)
        if (! d.empty() && seen.insert (d).second) out += (out.empty() ? "" : std::string (1, sep)) + d;
    // The system folders always (the CLIs call tools such as /usr/bin/security for the login keychain).
#if ! JUCE_WINDOWS
    for (const char* d : { "/usr/bin", "/bin", "/usr/sbin", "/sbin" })
        if (seen.insert (d).second) out += std::string (1, sep) + d;
#endif
    return out;
}
} // namespace

AutoAgent::~AutoAgent() { kill(); }

void AutoAgent::rememberAgent (const std::string& client, const std::string& path)
{
    const json j = { { "client", client }, { "path", path }, { "time", (double) juce::Time::currentTimeMillis() / 1000.0 } };
    agentFile().replaceWithText (j.dump (1));
}

json AutoAgent::lastAgent()
{
    auto j = json::parse (agentFile().loadFileAsString().toStdString(), nullptr, false);
    return j.is_object() ? j : json::object();
}

std::optional<AutoAgent::Cli> AutoAgent::find (const json& agentInfo)
{
    if (const char* off = std::getenv ("MFA_AUTO_AGENT"); off && std::string (off) == "0") return std::nullopt; // turned off
    {
        std::lock_guard<std::mutex> lk (m);
        if (nowMs() - findAtMs < 10000.0) return cached;
    }
    const auto last = lastAgent();
    std::vector<std::string> kinds;
    for (auto k : { kindOf (agentInfo.value ("client", std::string())), kindOf (last.value ("client", std::string())), std::string ("claude"), std::string ("codex") })
        if (! k.empty() && std::find (kinds.begin(), kinds.end(), k) == kinds.end()) kinds.push_back (k);
    const auto agentPath = agentInfo.value ("path", last.value ("path", std::string()));
    const auto dirs = searchDirs (agentPath);
    std::optional<Cli> found;
    for (auto& k : kinds)
    {
        for (auto& d : dirs)
        {
#if JUCE_WINDOWS
            for (auto ext : { ".exe", ".cmd" })
            {
                auto f = juce::File (juce::String (d)).getChildFile (juce::String (k) + ext);
                if (f.existsAsFile()) { found = Cli { k, f.getFullPathName().toStdString(), joinPath (dirs) }; break; }
            }
#else
            auto f = juce::File (juce::String (d)).getChildFile (juce::String (k));
            if (f.existsAsFile() && ::access (f.getFullPathName().toRawUTF8(), X_OK) == 0) found = Cli { k, f.getFullPathName().toStdString(), joinPath (dirs) };
#endif
            if (found) break;
        }
        if (found) break;
    }
    std::lock_guard<std::mutex> lk (m);
    cached = found;
    findAtMs = nowMs();
    return found;
}

bool AutoAgent::alive()
{
    // (m held)
#if JUCE_WINDOWS
    if (! proc) return false;
    if (WaitForSingleObject ((HANDLE) proc, 0) == WAIT_TIMEOUT) return true;
    CloseHandle ((HANDLE) proc);
    proc = nullptr;
    return false;
#else
    if (pid <= 0) return false;
    int st = 0;
    if (::waitpid (pid, &st, WNOHANG) == 0) return true;
    pid = 0;
    return false;
#endif
}

bool AutoAgent::launch (const Cli& cli, const std::string& folder, const std::string& selfExe)
{
    // (m held)
    auto logs = im::appDataDir().getChildFile ("logs");
    logs.createDirectory();
    logFile = logs.getChildFile ("agent-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".log").getFullPathName().toStdString();
    auto work = im::appDataDir().getChildFile ("agent-work"); // an empty folder to run in (nothing to touch there)
    work.createDirectory();
    std::vector<std::string> args;
    std::string allowed;
    for (auto* t : kTools) allowed += (allowed.empty() ? "" : ",") + std::string ("mcp__") + kServer + "__" + t;
    const char* model = std::getenv ("MFA_AGENT_MODEL"); // tests: a cheaper model
    if (cli.kind == "claude")
    {
        auto cfg = im::appDataDir().getChildFile ("agent-mcp.json");
        cfg.replaceWithText (json { { "mcpServers", { { kServer, { { "command", selfExe }, { "args", json::array ({ "mcp" }) } } } } } }.dump (1));
        args = { "-p", prompt (folder), "--mcp-config", cfg.getFullPathName().toStdString(), "--strict-mcp-config", "--allowedTools", allowed };
        if (model && *model) args.insert (args.end(), { "--model", model });
    }
    else
    {
        const auto q = [] (const std::string& s) { return "\"" + juce::String (s).replace ("\\", "\\\\").replace ("\"", "\\\"").toStdString() + "\""; };
        const std::string key = std::string ("mcp_servers.") + kServer;
        // The mix tools are approved per server; no other approvals (and none persisted: --approve-for-me would mark the
        // work folder trusted in the user's Codex config). Read-only sandbox: the agent needs no shell or files.
        args = { "exec", "--ephemeral", "--skip-git-repo-check", "-s", "read-only",
                 "-c", key + ".command=" + q (selfExe), "-c", key + ".args=[\"mcp\"]", "-c", key + ".tool_timeout_sec=120",
                 "-c", key + ".default_tools_approval_mode=\"approve\"" };
        if (model && *model) args.insert (args.end(), { "-m", model });
        args.push_back (prompt (folder));
    }
    {
        std::string line = "started: " + cli.exe;
        for (auto& a : args) line += " '" + a + "'";
        juce::File (juce::String (logFile)).replaceWithText (line + "\nPATH=" + cli.path + "\n\n");
    }
#if JUCE_WINDOWS
    auto quoted = [] (const std::string& v) { return std::wstring (L"\"") + juce::String (v).replace ("\"", "\\\"").toWideCharPointer() + L"\""; };
    std::wstring cmd;
    const bool script = juce::String (cli.exe).endsWithIgnoreCase (".cmd");
    if (script) cmd = L"cmd.exe /d /s /c \"";
    cmd += quoted (cli.exe);
    for (auto& a : args) cmd += L" " + quoted (a);
    if (script) cmd += L"\"";
    SetEnvironmentVariableW (L"PATH", juce::String (cli.path).toWideCharPointer());
    SECURITY_ATTRIBUTES sa { sizeof (sa), nullptr, TRUE };
    HANDLE log = CreateFileW (juce::String (logFile).toWideCharPointer(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si {};
    si.cb = sizeof (si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi {};
    const bool ok = CreateProcessW (nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                    work.getFullPathName().toWideCharPointer(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle (log);
    if (! ok) return false;
    CloseHandle (pi.hThread);
    proc = pi.hProcess;
    return true;
#else
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init (&fa);
    posix_spawn_file_actions_addopen (&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen (&fa, 1, logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2 (&fa, 1, 2);
    posix_spawn_file_actions_addchdir_np (&fa, work.getFullPathName().toRawUTF8());
    posix_spawnattr_t at;
    posix_spawnattr_init (&at);
    posix_spawnattr_setflags (&at, POSIX_SPAWN_SETPGROUP); // its own process group: Stop ends it and what it started
    posix_spawnattr_setpgroup (&at, 0);
    std::vector<std::string> all { cli.exe };
    all.insert (all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : all) argv.push_back (const_cast<char*> (a.c_str()));
    argv.push_back (nullptr);
    std::vector<std::string> envs;
    for (char** e = environ; e && *e; ++e)
        if (std::strncmp (*e, "PATH=", 5) != 0) envs.emplace_back (*e);
    envs.push_back ("PATH=" + cli.path);
    std::vector<char*> envp;
    for (auto& e : envs) envp.push_back (const_cast<char*> (e.c_str()));
    envp.push_back (nullptr);
    pid_t child = 0;
    const int rc = posix_spawn (&child, cli.exe.c_str(), &fa, &at, argv.data(), envp.data());
    posix_spawn_file_actions_destroy (&fa);
    posix_spawnattr_destroy (&at);
    if (rc != 0) return false;
    pid = child;
    return true;
#endif
}

void AutoAgent::kill()
{
    std::lock_guard<std::mutex> lk (m);
#if JUCE_WINDOWS
    if (proc)
    {
        TerminateProcess ((HANDLE) proc, 1);
        CloseHandle ((HANDLE) proc);
        proc = nullptr;
    }
#else
    if (pid > 0)
    {
        ::kill (-pid, SIGTERM);
        for (int k = 0; k < 20 && ::waitpid (pid, nullptr, WNOHANG) == 0; ++k) juce::Thread::sleep (50);
        if (::waitpid (pid, nullptr, WNOHANG) == 0) { ::kill (-pid, SIGKILL); ::waitpid (pid, nullptr, 0); }
        pid = 0;
    }
#endif
    if (state == "starting" || state == "running")
    {
        state = "exited";
        exitedMs = nowMs();
    }
}

void AutoAgent::tick (im::InteractiveMixerProcessor& live, const std::string& folder, const std::string& selfExe, const json& agentInfo)
{
    auto& bridge = im::Bridge::shared();
    const auto ts = live.agent.turnStatus();
    const bool waiting = ts.stage == im::Agent::Stage::Waiting;
    std::optional<Cli> cli;
    {
        std::lock_guard<std::mutex> lk (m);
        const bool wasAlive = state == "starting" || state == "running";
        if (wasAlive && ! alive())
        {
            state = "exited";
            exitedMs = nowMs();
        }
        if (state == "starting" && bridge.secondsSinceAgentSeen() * 1000.0 < nowMs() - sinceMs) state = "running"; // it polled
        if (! waiting || ts.tick == abortedTick) return;
        if (state == "starting" || state == "running") return; // ours is on it
        if (launchedTick == ts.tick)
        {
            // We started an agent for this turn and it ended without answering: give up on the turn, say why.
            abortedTick = ts.tick;
            // Its last words usually say why (e.g. "Not logged in · Please run /login").
            juce::StringArray lines;
            lines.addLines (juce::File (juce::String (logFile)).loadFileAsString());
            lines.removeEmptyStrings();
            const auto said = lines.size() > 2 ? lines[lines.size() - 1].trim().substring (0, 200) : juce::String();
            error = "the coding agent (" + cliKind + ") exited without answering this turn"
                    + (said.isNotEmpty() ? " (it said: " + said.toStdString() + ")" : std::string()) + "; log: " + logFile;
            bridge.abortPending (error);
            return;
        }
        // A connected agent will fetch it, unless the last one seen was ours and it has exited since.
        const double lastSeenMs = nowMs() - bridge.secondsSinceAgentSeen() * 1000.0;
        const bool oursWasLast = state == "exited" && exitedMs >= lastSeenMs;
        if (bridge.pendingFetched() || (bridge.secondsSinceAgentSeen() < 60.0 && ! oursWasLast)) return;
        if (ts.sinceSec < 5.0) return;
    }
    cli = find (agentInfo);
    std::lock_guard<std::mutex> lk (m);
    if (! cli)
    {
        state = "unavailable";
        error = "Claude Code or Codex not found";
        if (ts.sinceSec >= 30.0)
        {
            abortedTick = ts.tick;
            bridge.abortPending ("no coding agent is connected, and Claude Code or Codex could not be found to start one. "
                                 "In Claude Code or Codex, ask it to answer the mixer (it uses the mix tools), then ask again.");
        }
        return;
    }
    launchedTick = ts.tick;
    cliKind = cli->kind;
    cliExe = cli->exe;
    error.clear();
    sinceMs = nowMs();
    if (launch (*cli, folder, selfExe)) state = "starting";
    else
    {
        state = "exited";
        exitedMs = nowMs();
        error = "could not start " + cli->exe;
    }
}

json AutoAgent::status() const
{
    std::lock_guard<std::mutex> lk (m);
    const double ref = state == "exited" ? exitedMs : sinceMs;
    return { { "state", state }, { "cli", cliKind.empty() ? json (nullptr) : json (cliKind) }, { "exe", cliExe.empty() ? json (nullptr) : json (cliExe) },
             { "since_s", state == "none" ? 0.0 : (nowMs() - ref) / 1000.0 }, { "error", error.empty() ? json (nullptr) : json (error) },
             { "log", logFile.empty() ? json (nullptr) : json (logFile) } };
}
} // namespace mfa
