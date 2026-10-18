#include "Instances.h"
#include "core/Paths.h"
#include <nlohmann/json.hpp>
#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <csignal>
 #include <fcntl.h>
 #include <spawn.h>
 #include <sys/stat.h>
 #include <unistd.h>
extern char** environ;
#endif

namespace mfa
{
using json = nlohmann::json;

namespace
{
juce::File dir()
{
    auto d = im::appDataDir().getChildFile ("instances");
    d.createDirectory();
    return d;
}

juce::File fileFor (const std::string& folder)
{
    return dir().getChildFile (juce::String::toHexString (juce::String (normaliseFolder (folder)).hashCode64()) + ".json");
}
} // namespace

std::string normaliseFolder (const std::string& folder)
{
    auto f = juce::File (juce::String (folder));
    auto s = f.getFullPathName();
    if (f.isSymbolicLink()) s = f.getLinkedTarget().getFullPathName();
    while (s.length() > 1 && (s.endsWithChar ('/') || s.endsWithChar ('\\'))) s = s.dropLastCharacters (1);
    return s.toStdString();
}

void writeInstance (const Instance& i)
{
    const json j = { { "pid", i.pid }, { "port", i.port }, { "token", i.token }, { "folder", normaliseFolder (i.folder) }, { "url", i.url } };
    auto f = fileFor (i.folder);
    f.replaceWithText (j.dump (1));
#if ! JUCE_WINDOWS
    ::chmod (f.getFullPathName().toRawUTF8(), 0600);
#endif
}

void removeInstance (const Instance& i)
{
    auto f = fileFor (i.folder);
    auto j = json::parse (f.loadFileAsString().toStdString(), nullptr, false);
    if (! j.is_discarded() && j.value ("pid", 0) == i.pid) f.deleteFile(); // never remove a newer instance's file
}

bool processAlive (int pid)
{
    if (pid <= 0) return false;
#if JUCE_WINDOWS
    HANDLE h = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (h == nullptr) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess (h, &code) && code == STILL_ACTIVE;
    CloseHandle (h);
    return alive;
#else
    return ::kill (pid, 0) == 0;
#endif
}

int currentPid()
{
#if JUCE_WINDOWS
    return (int) GetCurrentProcessId();
#else
    return (int) ::getpid();
#endif
}

std::vector<Instance> liveInstances()
{
    std::vector<Instance> out;
    for (auto& f : dir().findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        auto j = json::parse (f.loadFileAsString().toStdString(), nullptr, false);
        if (j.is_discarded()) continue;
        Instance i { j.value ("pid", 0), j.value ("port", 0), j.value ("token", std::string()), j.value ("folder", std::string()), j.value ("url", std::string()) };
        if (! processAlive (i.pid))
        {
            f.deleteFile();
            continue;
        }
        out.push_back (i);
    }
    return out;
}

std::optional<Instance> instanceFor (const std::string& folder)
{
    const auto want = normaliseFolder (folder);
    for (auto& i : liveInstances())
        if (i.folder == want) return i;
    return std::nullopt;
}

bool spawnDetached (const std::string& exe, const std::vector<std::string>& args, const std::string& logFile)
{
#if JUCE_WINDOWS
    std::wstring cmd = L"\"" + juce::String (exe).toWideCharPointer() + std::wstring (L"\"");
    for (auto& a : args) cmd += L" \"" + std::wstring (juce::String (a).toWideCharPointer()) + L"\"";
    SECURITY_ATTRIBUTES sa { sizeof (sa), nullptr, TRUE };
    HANDLE log = CreateFileW (juce::String (logFile).toWideCharPointer(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si {};
    si.cb = sizeof (si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    PROCESS_INFORMATION pi {};
    const bool ok = CreateProcessW (nullptr, cmd.data(), nullptr, nullptr, TRUE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle (log);
    if (ok)
    {
        CloseHandle (pi.hThread);
        CloseHandle (pi.hProcess);
    }
    return ok;
#else
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init (&fa);
    posix_spawn_file_actions_addopen (&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen (&fa, 1, logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2 (&fa, 1, 2);
    posix_spawnattr_t at;
    posix_spawnattr_init (&at);
    posix_spawnattr_setflags (&at, POSIX_SPAWN_SETSID); // own session: survives the agent session that started it
    std::vector<std::string> all { exe };
    all.insert (all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : all) argv.push_back (const_cast<char*> (a.c_str()));
    argv.push_back (nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn (&pid, exe.c_str(), &fa, &at, argv.data(), environ);
    posix_spawn_file_actions_destroy (&fa);
    posix_spawnattr_destroy (&at);
    return rc == 0;
#endif
}
} // namespace mfa
