#include "agent/Bridge.h"
#include <juce_core/juce_core.h>
#include <chrono>
#include <set>

namespace im
{
using json = nlohmann::json;

namespace
{
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
} // namespace

Bridge& Bridge::shared()
{
    static Bridge b;
    return b;
}

std::string Bridge::renderText (const json& request)
{
    std::string out;
    for (auto& msg : request.value ("messages", json::array()))
    {
        std::string role = msg.value ("role", std::string());
        for (auto& c : role) c = (char) std::toupper ((unsigned char) c);
        const auto& content = msg.contains ("content") && msg["content"].is_string() ? msg["content"].get<std::string>() : std::string();
        out += "=== " + role + " ===\n" + content + "\n\n";
    }
    out += "=== TOOLS (function schemas) ===\n" + request.value ("tools", json::array()).dump (1) + "\n";
    return out;
}

Bridge::Result Bridge::ask (const json& request, const std::string& trigger, const std::string& userMsg, double timeoutSec)
{
    std::unique_lock<std::mutex> lk (m);
    if (cancelled) return { false, "cancelled", {} };
    pending = Turn { nextId++, trigger, userMsg, request, renderText (request) };
    reply.reset();
    const int id = pending->id;
    cv.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds ((long long) (timeoutSec * 1000));
    cv.wait_until (lk, deadline, [&] { return reply.has_value() || cancelled; });
    Result r;
    if (reply)
    {
        // Wrap {"content", "tool_calls": [{name, arguments}]} as an OpenAI chat completion.
        json calls = json::array();
        int k = 0;
        for (auto& c : reply->value ("tool_calls", json::array()))
            calls.push_back ({ { "id", "call_" + std::to_string (id) + "_" + std::to_string (k++) }, { "type", "function" },
                               { "function", { { "name", c.value ("name", std::string()) }, { "arguments", c.value ("arguments", json::object()).dump() } } } });
        r.ok = true;
        r.response = { { "choices", json::array ({ { { "index", 0 }, { "finish_reason", calls.empty() ? "stop" : "tool_calls" },
                                                     { "message", { { "role", "assistant" }, { "content", reply->value ("content", std::string()) },
                                                                    { "tool_calls", calls } } } } }) },
                       { "usage", { { "prompt_tokens", 0 }, { "completion_tokens", 0 } } } };
    }
    else
        r.error = cancelled ? "cancelled" : "no answer from the coding agent within " + std::to_string ((int) timeoutSec) + " s";
    pending.reset();
    reply.reset();
    cv.notify_all();
    return r;
}

void Bridge::cancel()
{
    std::lock_guard<std::mutex> lk (m);
    cancelled = true;
    cv.notify_all();
}

std::optional<Bridge::Turn> Bridge::waitTurn (double waitSec)
{
    lastSeenMs = nowMs();
    std::unique_lock<std::mutex> lk (m);
    cv.wait_for (lk, std::chrono::milliseconds ((long long) (waitSec * 1000)), [&] { return (pending.has_value() && ! reply) || cancelled; });
    lastSeenMs = nowMs();
    if (pending && ! reply) return pending;
    return std::nullopt;
}

std::string Bridge::answer (int turnId, const json& r)
{
    lastSeenMs = nowMs();
    std::lock_guard<std::mutex> lk (m);
    if (! pending || reply) return "no turn is waiting for an answer";
    if (pending->id != turnId) return "turn " + std::to_string (turnId) + " is not the waiting turn (" + std::to_string (pending->id) + ")";
    if (! r.is_object()) return "the answer must be an object {content, tool_calls}";
    std::set<std::string> known;
    for (auto& t : pending->request.value ("tools", json::array()))
        if (t.contains ("function")) known.insert (t["function"].value ("name", std::string()));
    const auto calls = r.value ("tool_calls", json::array());
    if (! calls.is_array()) return "tool_calls must be a list";
    for (size_t i = 0; i < calls.size(); ++i)
    {
        const auto& c = calls[i];
        if (! c.is_object() || ! c.contains ("name") || ! c["name"].is_string())
            return "tool_calls[" + std::to_string (i) + "] needs a string \"name\"";
        if (! known.count (c["name"].get<std::string>()))
            return "tool_calls[" + std::to_string (i) + "]: unknown tool \"" + c["name"].get<std::string>() + "\"";
        if (c.contains ("arguments") && ! c["arguments"].is_object())
            return "tool_calls[" + std::to_string (i) + "]: \"arguments\" must be an object";
    }
    reply = r;
    cv.notify_all();
    return {};
}

double Bridge::secondsSinceAgentSeen() const
{
    const double t = lastSeenMs.load();
    return t < 0 ? 1e9 : (nowMs() - t) / 1000.0;
}

bool Bridge::hasPending() const
{
    std::lock_guard<std::mutex> lk (m);
    return pending.has_value() && ! reply;
}
} // namespace im
