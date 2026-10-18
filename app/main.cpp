// mix_final_ACTUAL.wav: a local mixing console in the browser, mixed by the user's coding agent.
//
//   mix-final-actual serve TRACKS_DIR [--port N] [--no-open] [--turns N] [--listen S] [--no-first-mix]
//       First mix (offline, N turns answered by the coding agent), then the live engine plays to the sound card and
//       the page opens. Prints the page URL; writes the instance file the MCP front reads.
//   mix-final-actual mcp
//       MCP server on stdin/stdout for Claude Code, Codex and other coding agents (see Mcp.cpp).
//   mix-final-actual TRACKS_DIR ...   same as serve
#include "AppState.h"
#include "FirstMix.h"
#include "Instances.h"
#include "Mcp.h"
#include "PluginProcessor.h"
#include "WebServer.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <csignal>
#include <iostream>
#include <random>

namespace
{
std::atomic<bool> quit { false };
void onSignal (int) { quit = true; }

std::string randomToken()
{
    std::random_device rd; // OS entropy: the token is what keeps other web pages away from the API
    std::string s;
    for (int i = 0; i < 8; ++i) s += juce::String::toHexString ((int) rd()).paddedLeft ('0', 8).toStdString();
    return s;
}

int usage()
{
    std::cerr << "usage: mix-final-actual serve TRACKS_DIR [--port N] [--no-open] [--turns N] [--listen S] [--no-first-mix]\n"
                 "       mix-final-actual mcp\n";
    return 2;
}

int serve (int argc, char** argv, int first)
{
    juce::File dir;
    int port = 0, turns = 5;
    double listen = 20.0;
    bool open = true, firstMix = true;
    for (int i = first; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::atoi (argv[++i]);
        else if (a == "--turns" && i + 1 < argc) turns = std::clamp (std::atoi (argv[++i]), 1, 10);
        else if (a == "--listen" && i + 1 < argc) listen = std::clamp (std::atof (argv[++i]), 4.0, 120.0);
        else if (a == "--no-open") open = false;
        else if (a == "--no-first-mix") firstMix = false;
        else dir = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (a));
    }
    if (! dir.isDirectory()) return usage();
    if (const char* e = std::getenv ("MFA_NO_OPEN"); e != nullptr && std::string (e) == "1") open = false; // tests

    mfa::AppState app;
    app.folder = dir.getFullPathName().toStdString();
    app.first.turns = turns;
    const auto token = randomToken();
    mfa::WebServer web (app, token);
    const int bound = web.start (port);
    if (bound < 0)
    {
        std::cerr << "cannot listen on 127.0.0.1:" << port << "\n";
        return 1;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string (bound) + "/?t=" + token;
    mfa::Instance inst { mfa::currentPid(), bound, token, app.folder, url };
    mfa::writeInstance (inst);
    std::cout << "mix_final_ACTUAL.wav: " << url << std::endl;
    std::signal (SIGINT, onSignal);
    std::signal (SIGTERM, onSignal);

    // 1. first mix, offline, before the page opens
    juce::MemoryBlock state;
    if (firstMix)
    {
        std::cout << "first mix: " << turns << " turns; waiting for the coding agent (mix tools: next_turn / submit)" << std::endl;
        if (auto err = mfa::runFirstMix (dir, turns, listen, app.first, state, quit); ! err.empty())
        {
            {
                std::lock_guard<std::mutex> lk (app.errMutex);
                app.error = err;
            }
            std::cerr << "first mix: " << err << "\n";
            if (quit)
            {
                mfa::removeInstance (inst);
                return 1;
            }
        }
    }
    app.first.done = true;

    // 2. the live engine, with the first mix loaded
    auto proc = std::make_unique<im::InteractiveMixerProcessor>();
    juce::AudioDeviceManager devices;
    if (auto err = devices.initialiseWithDefaultDevices (0, 2); err.isNotEmpty())
    {
        std::cerr << "audio device: " << err << "\n";
        mfa::removeInstance (inst);
        return 1;
    }
    juce::AudioProcessorPlayer player;
    player.setProcessor (proc.get());
    devices.addAudioCallback (&player);
    if (auto err = proc->loadSessionFolder (dir); err.isNotEmpty()) // prepareToPlay has run: loads at the device rate
    {
        std::cerr << "load " << dir.getFullPathName() << ": " << err << "\n";
        mfa::removeInstance (inst);
        return 1;
    }
    if (state.getSize() > 0) proc->setStateInformation (state.getData(), (int) state.getSize());
    proc->player.setLooping (true);
    app.live = proc.get();
    std::cout << proc->player.numLoadedTracks() << " tracks live; page: " << url << std::endl;
    if (open) juce::URL (url).launchInDefaultBrowser();

    // runDispatchLoop() returns at once in a console app on macOS, so pump the message thread in slices.
    while (! quit) juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    app.live = nullptr;
    proc->agent.stop();
    web.stop();
    devices.removeAudioCallback (&player);
    player.setProcessor (nullptr);
    mfa::removeInstance (inst);
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "mcp") return mfa::runMcp (argv[0]);
    juce::ScopedJuceInitialiser_GUI gui;
    if (cmd == "serve") return serve (argc, argv, 2);
    if (cmd == "-h" || cmd == "--help") return usage();
    return serve (argc, argv, 1);
}
