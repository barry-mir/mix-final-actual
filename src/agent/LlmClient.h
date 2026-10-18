#pragma once
// Minimal OpenAI-compatible client for llama-server (/health, /v1/chat/completions,
// /tokenize). Blocking calls; used only from the agent worker / server thread.
#include <nlohmann/json.hpp>
#include <mutex>
#include <string>

namespace httplib { class Client; }

namespace im
{
// Lets another thread abort an in-flight request (closes its socket).
class CancelToken
{
public:
    void cancel();
    void reset();
    bool cancelled() const;
    void attach (httplib::Client* c); // returns immediately if already cancelled
    void detach();

private:
    mutable std::mutex m;
    httplib::Client* active = nullptr;
    bool isCancelled = false;
};

struct LlmResult
{
    bool ok = false;
    int status = 0;
    std::string error;
    nlohmann::json response;
    double wallMs = 0;
};

class LlmClient
{
public:
    LlmClient (std::string host, int port) : host (std::move (host)), port (port) {}
    bool health (int timeoutMs = 500) const;
    LlmResult chat (const nlohmann::json& request, int timeoutSec = 300, CancelToken* cancel = nullptr) const; // CPU cold prefill can take ~110 s under load
    int tokenize (const std::string& text, int timeoutMs = 2000) const; // -1 on failure
    // Tokens of the fully templated prompt (chat template + tools) via /apply-template + /tokenize; -1 on failure.
    int templatedTokens (const nlohmann::json& messages, int timeoutMs = 3000) const;

    // Request body used for every tick (spec §8).
    static nlohmann::json makeRequest (const nlohmann::json& messages, int maxTokens = 400, bool enableThinking = false);

private:
    std::string host;
    int port;
};
} // namespace im
