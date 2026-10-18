#include "agent/LlmClient.h"
#include "agent/Tools.h"
#include <httplib.h>
#include <chrono>

namespace im
{
bool LlmClient::health (int timeoutMs) const
{
    httplib::Client cli (host, port);
    cli.set_connection_timeout (0, timeoutMs * 1000);
    cli.set_read_timeout (0, timeoutMs * 1000);
    auto res = cli.Get ("/health");
    return res && res->status == 200;
}

nlohmann::json LlmClient::makeRequest (const nlohmann::json& messages, int maxTokens, bool enableThinking)
{
    return { { "model", "local" },
             { "messages", messages },
             { "tools", toolSchemas() },
             { "tool_choice", "auto" },
             { "parallel_tool_calls", true },
             { "temperature", 0.3 },
             { "max_tokens", maxTokens },
             { "cache_prompt", true },
             { "chat_template_kwargs", { { "enable_thinking", enableThinking } } },
             { "stream", false } };
}

void CancelToken::cancel()
{
    std::lock_guard<std::mutex> lk (m);
    isCancelled = true;
    if (active) active->stop();
}

void CancelToken::reset()
{
    std::lock_guard<std::mutex> lk (m);
    isCancelled = false;
}

bool CancelToken::cancelled() const
{
    std::lock_guard<std::mutex> lk (m);
    return isCancelled;
}

void CancelToken::attach (httplib::Client* c)
{
    std::lock_guard<std::mutex> lk (m);
    active = c;
}

void CancelToken::detach()
{
    std::lock_guard<std::mutex> lk (m);
    active = nullptr;
}

LlmResult LlmClient::chat (const nlohmann::json& request, int timeoutSec, CancelToken* cancel) const
{
    LlmResult r;
    const auto t0 = std::chrono::steady_clock::now();
    httplib::Client cli (host, port);
    cli.set_connection_timeout (2, 0);
    cli.set_read_timeout (timeoutSec, 0);
    cli.set_write_timeout (10, 0);
    if (cancel)
    {
        if (cancel->cancelled())
        {
            r.error = "cancelled";
            return r;
        }
        cancel->attach (&cli);
    }
    auto res = cli.Post ("/v1/chat/completions", request.dump(), "application/json");
    if (cancel)
    {
        cancel->detach();
        if (cancel->cancelled())
        {
            r.error = "cancelled";
            return r;
        }
    }
    r.wallMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    if (! res)
    {
        r.error = "http error: " + httplib::to_string (res.error());
        return r;
    }
    r.status = res->status;
    r.response = nlohmann::json::parse (res->body, nullptr, false);
    if (res->status != 200)
    {
        r.error = "status " + std::to_string (res->status) + ": " + res->body.substr (0, 300);
        return r;
    }
    if (r.response.is_discarded())
    {
        r.error = "invalid JSON body";
        return r;
    }
    r.ok = true;
    return r;
}

int LlmClient::templatedTokens (const nlohmann::json& messages, int timeoutMs) const
{
    httplib::Client cli (host, port);
    cli.set_connection_timeout (0, timeoutMs * 1000);
    cli.set_read_timeout (0, timeoutMs * 1000);
    const nlohmann::json body = { { "messages", messages }, { "tools", toolSchemas() },
                                  { "chat_template_kwargs", { { "enable_thinking", false } } } };
    auto res = cli.Post ("/apply-template", body.dump(), "application/json");
    if (! res || res->status != 200) return -1;
    auto j = nlohmann::json::parse (res->body, nullptr, false);
    if (j.is_discarded() || ! j.contains ("prompt") || ! j["prompt"].is_string()) return -1;
    return tokenize (j["prompt"].get<std::string>(), timeoutMs);
}

int LlmClient::tokenize (const std::string& text, int timeoutMs) const
{
    httplib::Client cli (host, port);
    cli.set_connection_timeout (0, timeoutMs * 1000);
    cli.set_read_timeout (0, timeoutMs * 1000);
    auto res = cli.Post ("/tokenize", nlohmann::json ({ { "content", text } }).dump(), "application/json");
    if (! res || res->status != 200) return -1;
    auto j = nlohmann::json::parse (res->body, nullptr, false);
    if (j.is_discarded() || ! j.contains ("tokens")) return -1;
    return (int) j["tokens"].size();
}
} // namespace im
