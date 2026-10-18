#include "agent/ServerManager.h"
#include <httplib.h>
#include <spawn.h>
#include <fcntl.h>
#include <thread>
#if JUCE_MAC
 #include <libproc.h>
 #include <sys/resource.h>
 #include <sys/sysctl.h>
#endif

extern char** environ;

namespace im
{
ServerManager::ServerManager() : juce::Thread ("IM llama-server monitor") {}
ServerManager::~ServerManager() { stopMonitoring(); }

const char* ServerManager::statusName (Status s)
{
    switch (s)
    {
        case Status::Online: return "online";
        case Status::Starting: return "starting";
        default: return "offline";
    }
}

void ServerManager::configure (const Settings& s)
{
    std::lock_guard<std::mutex> lk (m);
    cfg = s;
    portNow = s.port;
}

std::string ServerManager::lastError() const
{
    std::lock_guard<std::mutex> lk (m);
    return err;
}

void ServerManager::startMonitoring()
{
    if (! isThreadRunning()) startThread (juce::Thread::Priority::low);
}

void ServerManager::stopMonitoring()
{
    if (isThreadRunning()) stopThread (3000);
}

int ServerManager::defaultThreads()
{
    int physical = 0;
#if JUCE_MAC
    size_t sz = sizeof (physical);
    if (sysctlbyname ("hw.physicalcpu", &physical, &sz, nullptr, 0) != 0) physical = 0;
#endif
    if (physical <= 0) physical = juce::SystemStats::getNumPhysicalCpus();
    return std::max (1, physical - 2);
}

juce::File ServerManager::logsDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
#if JUCE_MAC
                 .getChildFile ("Application Support")
#endif
                 .getChildFile ("InteractiveMixer")
                 .getChildFile ("logs");
    d.createDirectory();
    return d;
}

std::vector<std::string> ServerManager::commandLine (const Settings& s)
{
    std::vector<std::string> args = { s.serverPath.empty() ? "llama-server" : s.serverPath,
             "-m", s.modelPath,
             "--host", "127.0.0.1",
             "--port", std::to_string (s.port),
             "-c", std::to_string (s.contextSize),
             "--cache-type-k", "q8_0",
             "--cache-type-v", "q8_0",
             "--threads", std::to_string (defaultThreads()),
             "-np", "1",
             "--jinja" };
    args.push_back ("-ngl");
    args.push_back (s.cpuOnly ? "0" : "99"); // 99 = every layer on the GPU (Metal)
    return args;
}

int ServerManager::probe (int p) const
{
    httplib::Client cli ("127.0.0.1", p);
    cli.set_connection_timeout (0, 300000);
    cli.set_read_timeout (1, 0);
    auto res = cli.Get ("/health");
    return res ? res->status : 0;
}

bool ServerManager::launch (const Settings& s)
{
    if (s.modelPath.empty() || ! juce::File (s.modelPath).existsAsFile())
    {
        std::lock_guard<std::mutex> lk (m);
        err = "model file not found: " + s.modelPath;
        return false;
    }
    // Resolve server binary: absolute path, or search PATH (+ Homebrew default).
    std::string bin = s.serverPath.empty() ? "llama-server" : s.serverPath;
    if (! juce::File::isAbsolutePath (bin))
    {
        for (auto dir : { "/opt/homebrew/bin", "/usr/local/bin" })
        {
            auto f = juce::File (dir).getChildFile (bin);
            if (f.existsAsFile()) { bin = f.getFullPathName().toStdString(); break; }
        }
    }
    auto args = commandLine (s);
    args[0] = bin;

    const auto logFile = logsDir().getChildFile ("llama-server.log").getFullPathName().toStdString();
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init (&fa);
    posix_spawn_file_actions_addopen (&fa, 1, logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2 (&fa, 1, 2);
    posix_spawn_file_actions_addopen (&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_t at;
    posix_spawnattr_init (&at);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags (&at, POSIX_SPAWN_SETSID); // detached: outlives the plugin, shared by instances
#endif
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back (const_cast<char*> (a.c_str()));
    argv.push_back (nullptr);
    pid_t pid = 0;
    const int rc = bin.find ('/') != std::string::npos ? posix_spawn (&pid, bin.c_str(), &fa, &at, argv.data(), environ)
                                                       : posix_spawnp (&pid, bin.c_str(), &fa, &at, argv.data(), environ);
    posix_spawn_file_actions_destroy (&fa);
    posix_spawnattr_destroy (&at);
    std::lock_guard<std::mutex> lk (m);
    if (rc != 0)
    {
        err = "failed to launch " + bin + " (errno " + std::to_string (rc) + ")";
        return false;
    }
    err.clear();
    return true;
}

int64_t ServerManager::queryRss (int p) const
{
    juce::ChildProcess lsof;
    if (! lsof.start (juce::StringArray { "/usr/sbin/lsof", "-nP", "-iTCP:" + juce::String (p), "-sTCP:LISTEN", "-t" })) return 0;
    const auto pid = lsof.readAllProcessOutput().trim().upToFirstOccurrenceOf ("\n", false, false);
    if (pid.isEmpty()) return 0;
#if JUCE_MAC
    // Physical footprint (what Activity Monitor shows as "Memory"); plain RSS under-reports
    // mmap'd / compressed model pages on macOS.
    rusage_info_v4 ri {};
    if (proc_pid_rusage (pid.getIntValue(), RUSAGE_INFO_V4, (rusage_info_t*) &ri) == 0) return (int64_t) ri.ri_phys_footprint;
#endif
    juce::ChildProcess ps;
    if (! ps.start (juce::StringArray { "/bin/ps", "-o", "rss=", "-p", pid })) return 0;
    return ps.readAllProcessOutput().trim().getLargeIntValue() * 1024;
}

void ServerManager::run()
{
    int rssCountdown = 0;
    while (! threadShouldExit())
    {
        Settings s;
        {
            std::lock_guard<std::mutex> lk (m);
            s = cfg;
        }
        const int code = probe (s.port);
        if (code == 200)
        {
            st = Status::Online;
            if (--rssCountdown <= 0)
            {
                rss = queryRss (s.port);
                rssCountdown = 3;
            }
        }
        else if (code != 0)
            st = Status::Starting; // server up, model loading (503)
        else
        {
            rss = 0;
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (s.autoStart && now - lastLaunchMs > 60000.0)
            {
                juce::InterProcessLock ipl ("InteractiveMixer-llama-server-" + juce::String (s.port));
                if (ipl.enter (0) && probe (s.port) == 0)
                {
                    lastLaunchMs = now;
                    st = launch (s) ? Status::Starting : Status::Offline;
                    // keep the lock while the server binds its port
                    for (int i = 0; i < 20 && probe (s.port) == 0 && ! threadShouldExit(); ++i) wait (250);
                }
                else
                    st = Status::Offline;
            }
            else if (st.load() != Status::Starting || now - lastLaunchMs > 120000.0)
                st = Status::Offline;
        }
        wait (2000);
    }
}
} // namespace im
