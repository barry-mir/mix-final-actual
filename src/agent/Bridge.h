#pragma once
// The bridge between the console's agent loop and the user's coding agent (Claude Code, Codex, ...): the loop posts
// one turn's request (system prompt, this turn's measurements and memory, tool schemas) and blocks; the coding agent
// fetches it through the MCP tools, answers with ALL of the turn's tool calls in one submit, and the loop goes on.
// No model runs inside the app and no API key is involved.
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

namespace im
{
class Bridge
{
public:
    // One per process: an app instance mixes one session, and its offline first-mix engine and the live engine
    // take turns on the same bridge.
    static Bridge& shared();

    struct Turn
    {
        int id = 0;
        std::string trigger;   // "first_mix" | "user" | "auto" | "retry"
        std::string userMsg;   // what the user typed in the page, if any
        nlohmann::json request; // OpenAI-style chat request: messages + tools
        std::string text;      // the same as plain text: === SYSTEM === / === USER === / === TOOLS ===
    };

    struct Result
    {
        bool ok = false;
        std::string error;
        nlohmann::json response; // OpenAI-style chat completion, ready for parseChatResponse
    };

    // Agent-loop side (worker thread). Blocks until the turn is answered, cancel() is called, or timeoutSec passes.
    Result ask (const nlohmann::json& request, const std::string& trigger, const std::string& userMsg, double timeoutSec);
    void cancel(); // aborts a waiting ask() (app shutdown)

    // Coding-agent side (MCP / HTTP threads).
    // The pending turn, waiting up to waitSec for one. The same turn is offered again until it is answered, so an
    // agent that lost a turn (crash, context reset) can fetch it again.
    std::optional<Turn> waitTurn (double waitSec);
    // reply: {"content": "...", "tool_calls": [{"name": "...", "arguments": {...}}]}. Returns "" if accepted, else
    // what is wrong with it (the turn stays pending, so the agent can fix and resubmit).
    std::string answer (int turnId, const nlohmann::json& reply);

    // Seconds since a coding agent last asked for a turn (large if never).
    double secondsSinceAgentSeen() const;
    bool hasPending() const;

    static std::string renderText (const nlohmann::json& request);

private:
    mutable std::mutex m;
    std::condition_variable cv;
    std::optional<Turn> pending;
    std::optional<nlohmann::json> reply;
    int nextId = 1;
    bool cancelled = false;
    std::atomic<double> lastSeenMs { -1.0 };
};
} // namespace im
