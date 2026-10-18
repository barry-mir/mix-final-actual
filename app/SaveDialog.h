#pragma once
// The operating system's own "save as" dialog, shown by a helper the OS already has (the console app has no windows
// of its own): macOS osascript, Windows PowerShell, Linux zenity or kdialog. Blocks until the user answers; call it
// from a worker thread (an HTTP handler), never the message thread.
#include <string>

namespace mfa
{
struct SaveChoice
{
    enum class Kind { Chosen, Cancelled, Unavailable } kind = Kind::Unavailable;
    std::string path;
};

SaveChoice chooseSaveFile (const std::string& suggestedPath);

// Shows a file in Finder / Explorer / the file manager.
void revealFile (const std::string& path);
} // namespace mfa
