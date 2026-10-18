#pragma once
// llama-server sidecar lifecycle (spec §8). A background thread probes
// http://127.0.0.1:{port}/health; if absent and auto-start is enabled it
// launches llama-server (detached, own session) with the pinned flags. The
// server is shared by all plugin instances (a running server on the port is
// reused; an inter-process lock prevents double launches). Never blocks the
// message or audio thread. Also samples the sidecar RSS for the settings panel.
#include "core/StateStore.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <mutex>

namespace im
{
class ServerManager : private juce::Thread
{
public:
    enum class Status { Offline, Starting, Online };

    ServerManager();
    ~ServerManager() override;

    void configure (const Settings& s); // thread-safe; picks up new path/port/model
    void startMonitoring();
    void stopMonitoring();

    Status status() const noexcept { return st.load(); }
    static const char* statusName (Status s);
    int64_t rssBytes() const noexcept { return rss.load(); }
    std::string lastError() const;
    int port() const noexcept { return portNow.load(); }

    static std::vector<std::string> commandLine (const Settings& s);
    static int defaultThreads();
    static juce::File logsDir();

private:
    void run() override;
    int probe (int port) const; // HTTP status of /health, 0 if unreachable
    bool launch (const Settings& s);
    int64_t queryRss (int port) const;

    mutable std::mutex m;
    Settings cfg;
    std::string err;
    std::atomic<Status> st { Status::Offline };
    std::atomic<int64_t> rss { 0 };
    std::atomic<int> portNow { 8089 };
    double lastLaunchMs = -1e9;
};
} // namespace im
