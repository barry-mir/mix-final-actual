#pragma once
// Local web UI and its JSON API, on 127.0.0.1 only. Every /api call needs the session token (?t=...), so other pages
// open in the browser cannot drive the mixer; the Host header must be a loopback name (DNS-rebinding guard).
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace im
{
class InteractiveMixerProcessor;
}

namespace bc
{
class WebServer
{
public:
    WebServer (im::InteractiveMixerProcessor& proc, std::string token);
    ~WebServer();

    // Binds 127.0.0.1:port (0 = any free port); returns the bound port, or -1.
    int start (int port);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::thread thread;
};
} // namespace bc
