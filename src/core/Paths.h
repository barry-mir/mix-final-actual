#pragma once
// Where the app keeps its own files (logs, the running instance's address): one folder per user.
#include <juce_core/juce_core.h>

namespace im
{
inline juce::File appDataDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
#if JUCE_MAC
                 .getChildFile ("Application Support")
#endif
                 .getChildFile ("mix-final-actual");
    d.createDirectory();
    return d;
}
} // namespace im
