#include "SaveDialog.h"
#include <juce_core/juce_core.h>

namespace mfa
{
namespace
{
// Runs a helper; returns its trimmed stdout and exit code, or false when it could not be started.
bool run (const juce::StringArray& args, juce::String& out, int& code)
{
    juce::ChildProcess p;
    if (! p.start (args, juce::ChildProcess::wantStdOut)) return false;
    out = p.readAllProcessOutput().trim(); // waits for the helper to exit (the user answering the dialog)
    p.waitForProcessToFinish (5000);
    code = (int) p.getExitCode();
    return true;
}

juce::String appleScriptString (const juce::String& s)
{
    return "\"" + s.replace ("\\", "\\\\").replace ("\"", "\\\"") + "\"";
}
} // namespace

SaveChoice chooseSaveFile (const std::string& suggested)
{
    const juce::File f { juce::String (suggested) };
    const auto dir = f.getParentDirectory();
    dir.createDirectory(); // the dialogs open in it
    SaveChoice c;
    juce::String out;
    int code = 0;
#if JUCE_MAC
    // "tell me to activate" brings osascript's dialog to the front without asking for any permission.
    const auto script = "tell me to activate\n"
                        "set f to choose file name with prompt \"Save the mix as\" default name "
                        + appleScriptString (f.getFileName()) + " default location (POSIX file " + appleScriptString (dir.getFullPathName()) + ")\n"
                        + "POSIX path of f";
    if (! run ({ "/usr/bin/osascript", "-e", script }, out, code)) return c;
#elif JUCE_WINDOWS
    const auto ps = juce::String ("Add-Type -AssemblyName System.Windows.Forms;"
                                  "$d = New-Object System.Windows.Forms.SaveFileDialog;"
                                  "$d.Title = 'Save the mix as';"
                                  "$d.Filter = 'WAV (*.wav)|*.wav|FLAC (*.flac)|*.flac|All files (*.*)|*.*';"
                                  "$d.OverwritePrompt = $true;")
                    + "$d.FileName = '" + f.getFileName().replace ("'", "''") + "';"
                    + "$d.InitialDirectory = '" + dir.getFullPathName().replace ("'", "''") + "';"
                    + "$o = New-Object System.Windows.Forms.Form -Property @{TopMost = $true};"
                      "if ($d.ShowDialog($o) -eq [System.Windows.Forms.DialogResult]::OK) { [Console]::Out.Write($d.FileName) } else { exit 1 }";
    // -EncodedCommand (UTF-16LE base64) avoids every quoting problem of the command line.
    juce::MemoryOutputStream utf16;
    for (auto ch : ps) { utf16.writeByte ((char) (ch & 0xff)); utf16.writeByte ((char) ((ch >> 8) & 0xff)); }
    const auto enc = juce::Base64::toBase64 (utf16.getData(), utf16.getDataSize());
    if (! run ({ "powershell.exe", "-NoProfile", "-STA", "-NonInteractive", "-EncodedCommand", enc }, out, code)) return c;
#else
    if (! run ({ "zenity", "--file-selection", "--save", "--confirm-overwrite", "--title=Save the mix as", "--filename=" + f.getFullPathName() }, out, code)
        && ! run ({ "kdialog", "--getsavefilename", f.getFullPathName(), "*.wav *.flac", "--title", "Save the mix as" }, out, code))
        return c;
#endif
    if (code == 0 && out.isNotEmpty())
    {
        c.kind = SaveChoice::Kind::Chosen;
        c.path = out.toStdString();
    }
    else
        c.kind = SaveChoice::Kind::Cancelled;
    return c;
}

void revealFile (const std::string& path)
{
    const juce::File f { juce::String (path) };
#if JUCE_MAC
    juce::ChildProcess p;
    if (f.exists()) p.start (juce::StringArray { "/usr/bin/open", "-R", f.getFullPathName() });
    else p.start (juce::StringArray { "/usr/bin/open", f.getParentDirectory().getFullPathName() });
    p.waitForProcessToFinish (3000);
#elif JUCE_WINDOWS
    juce::ChildProcess p;
    p.start ("explorer.exe /select,\"" + f.getFullPathName() + "\"");
#else
    juce::ChildProcess p;
    p.start (juce::StringArray { "xdg-open", (f.isDirectory() ? f : f.getParentDirectory()).getFullPathName() });
#endif
}
} // namespace mfa
