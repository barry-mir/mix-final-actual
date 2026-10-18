// mix_final_ACTUAL.wav: a local mixing console in the browser. The engine plays the tracks to the sound card in real time; the web
// page (served on 127.0.0.1) moves its parameters, so every change is heard at once.
//
// usage: mix-final-actual TRACKS_DIR [--port N] [--no-open]
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
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File dir;
    int port = 0;
    bool open = true;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::atoi (argv[++i]);
        else if (a == "--no-open") open = false;
        else dir = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (a));
    }
    if (! dir.isDirectory())
    {
        std::cerr << "usage: mix-final-actual TRACKS_DIR [--port N] [--no-open]\n";
        return 2;
    }

    auto proc = std::make_unique<im::InteractiveMixerProcessor>();
    juce::AudioDeviceManager devices;
    if (auto err = devices.initialiseWithDefaultDevices (0, 2); err.isNotEmpty())
    {
        std::cerr << "audio device: " << err << "\n";
        return 1;
    }
    juce::AudioProcessorPlayer player;
    player.setProcessor (proc.get());
    devices.addAudioCallback (&player);

    // prepareToPlay has run by now (the device started), so the session loads at the device rate.
    if (auto err = proc->loadSessionFolder (dir); err.isNotEmpty())
    {
        std::cerr << "load " << dir.getFullPathName() << ": " << err << "\n";
        return 1;
    }

    const auto token = randomToken();
    mfa::WebServer web (*proc, token);
    const int bound = web.start (port);
    if (bound < 0)
    {
        std::cerr << "cannot listen on 127.0.0.1:" << port << "\n";
        return 1;
    }
    const juce::String url = "http://127.0.0.1:" + juce::String (bound) + "/?t=" + juce::String (token);
    std::cout << proc->player.numLoadedTracks() << " tracks from " << dir.getFullPathName() << "\n"
              << "mix_final_ACTUAL.wav: " << url << std::endl;
    if (open) juce::URL (url).launchInDefaultBrowser();

    std::signal (SIGINT, onSignal);
    std::signal (SIGTERM, onSignal);
    // runDispatchLoop() returns at once in a console app on macOS, so pump the message thread in slices.
    while (! quit) juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    web.stop();
    devices.removeAudioCallback (&player);
    player.setProcessor (nullptr);
    return 0;
}
