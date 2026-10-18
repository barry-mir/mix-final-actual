#pragma once
// The project file next to the tracks ("mix_final_ACTUAL project.json"): every console setting, track names and groups,
// the agent's turns and chat, and the loop region. Saved automatically while the page is open; when it exists, the
// next start reopens it instead of making a new first mix.
#include <juce_core/juce_core.h>
#include <string>

namespace im
{
class InteractiveMixerProcessor;
}

namespace mfa
{
juce::File projectFile (const juce::File& tracksDir);
std::string projectJson (im::InteractiveMixerProcessor& p); // message thread
bool saveProject (im::InteractiveMixerProcessor& p, const juce::File& f);
std::string loadProject (im::InteractiveMixerProcessor& p, const juce::File& f); // "" or the error; message thread
} // namespace mfa
