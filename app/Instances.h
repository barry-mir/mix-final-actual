#pragma once
// Running app instances, one file each under <app data>/instances/: how the MCP front finds the instance that mixes
// a folder (port, token, page URL). The files hold the session token, so they are only readable by the user.
#include <optional>
#include <string>
#include <vector>

namespace mfa
{
struct Instance
{
    int pid = 0;
    int port = 0;
    std::string token;
    std::string folder;
    std::string url;
};

void writeInstance (const Instance& i);
void removeInstance (const Instance& i);
std::vector<Instance> liveInstances();                       // stale files (dead process) are removed
std::optional<Instance> instanceFor (const std::string& folder); // folder: absolute path, compared normalised
bool processAlive (int pid);
int currentPid();
// Starts `exe args...` detached from this process (own session, no inherited stdio; output to logFile).
bool spawnDetached (const std::string& exe, const std::vector<std::string>& args, const std::string& logFile);
std::string normaliseFolder (const std::string& folder);
// The instance mixing `folder`: the running one, else `exe serve folder extraArgs...` started detached (output to
// <appdata>/logs/serve.log), waiting up to waitSec for it to come up. nullopt + error if it does not.
std::optional<Instance> startOrAttach (const std::string& exe, const std::string& folder, const std::vector<std::string>& extraArgs,
                                       double waitSec, std::string& error);
} // namespace mfa
