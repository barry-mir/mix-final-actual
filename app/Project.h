#pragma once
// The project file next to the tracks ("mix_final_ACTUAL project.json"): every console setting, track names and groups,
// clip edits, the agent's turns and chat, the loop region, and the extras below. Saved automatically while the app
// runs; when it exists, the next start reopens it instead of making a new first mix.
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace im
{
class InteractiveMixerProcessor;
}

namespace mfa
{
struct ProjectExtras
{
    std::optional<double> loudness = -14.0; // playback/export loudness target (nullopt = as mixed)
    double loudnessGainDb = 0.0;            // the playback gain last measured (playback starts at it on reopen)
    std::string name;                       // the project's name ("" = the folder name)
    nlohmann::json tempo;                   // TempoState::toJson(), or null (never detected)
    bool analysisWhole = false;             // agent turns analyze the whole song
};

juce::File projectFile (const juce::File& tracksDir);
std::string projectJson (im::InteractiveMixerProcessor& p, const ProjectExtras& x); // message thread
bool saveProject (im::InteractiveMixerProcessor& p, const ProjectExtras& x, const juce::File& f);
// "" or the error; message thread. Fields missing from older projects keep the values x has.
std::string loadProject (im::InteractiveMixerProcessor& p, const juce::File& f, ProjectExtras& x);
// Only the extras (no engine), e.g. the name for the recent list.
std::optional<ProjectExtras> readProjectExtras (const juce::File& f);
} // namespace mfa
