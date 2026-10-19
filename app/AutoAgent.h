#pragma once
// Starts the user's coding agent by itself when a turn is waiting and no agent is answering (the agent that opened the
// session exited, or is busy with something else): Claude Code (`claude -p`) or Codex (`codex exec`), headless, with
// only the mix tools, on the user's own login (no API key). It answers the waiting turns as the mix skill says and
// exits when no turn has come for a while. One at a time; Stop kills it.
#include <nlohmann/json.hpp>
#include <mutex>
#include <optional>
#include <string>

namespace im
{
class InteractiveMixerProcessor;
}

namespace mfa
{
class AutoAgent
{
public:
    ~AutoAgent();

    struct Cli
    {
        std::string kind; // "claude" | "codex"
        std::string exe;  // full path
        std::string path; // PATH to run it with
    };
    // Claude Code or Codex: the kind the session's own agent is (MCP client name in agentInfo), else the last agent that
    // used the mix tools (<appdata>/agent.json), else claude, else codex. Looked up on that agent's PATH, this process's
    // PATH and the usual install folders. Cached for 10 s.
    std::optional<Cli> find (const nlohmann::json& agentInfo);
    // MCP side: remember which agent uses the mix tools, with its PATH (for find, and for the console it talks to).
    static void rememberAgent (const std::string& client, const std::string& path);
    static nlohmann::json lastAgent();

    // Main loop (~4 Hz): starts an agent for a turn that waits unanswered, reaps it, gives up on turns nobody can answer.
    void tick (im::InteractiveMixerProcessor& live, const std::string& folder, const std::string& selfExe, const nlohmann::json& agentInfo);
    void kill(); // Stop pressed, or shutdown
    nlohmann::json status() const; // {state: none|starting|running|exited|unavailable, cli, exe, since_s, error, log}

private:
    bool launch (const Cli& cli, const std::string& folder, const std::string& selfExe);
    bool alive();
    mutable std::mutex m;
    std::string state = "none", cliKind, cliExe, error, logFile;
    double sinceMs = 0, exitedMs = 0, findAtMs = -1e9;
    int launchedTick = -1, abortedTick = -1;
    std::optional<Cli> cached;
#if defined(_WIN32)
    void* proc = nullptr;
#else
    int pid = 0;
#endif
};
} // namespace mfa
