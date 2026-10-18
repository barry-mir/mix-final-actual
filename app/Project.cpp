#include "Project.h"
#include "PluginProcessor.h"
#include "agent/MixPriors.h"
#include <nlohmann/json.hpp>

namespace mfa
{
using json = nlohmann::json;

juce::File projectFile (const juce::File& tracksDir) { return tracksDir.getChildFile ("mix_final_ACTUAL project.json"); }

std::string projectJson (im::InteractiveMixerProcessor& p, std::optional<double> loudness)
{
    juce::MemoryBlock state;
    p.getStateInformation (state);
    json groups = json::object();
    for (auto& [name, role] : im::priors::roleOverrides()) groups[name] = role;
    return json { { "app", "mix-final-actual" }, { "version", 1 }, { "state", state.toBase64Encoding().toStdString() },
                  { "groups", groups }, { "loop", { p.player.loopStartSec(), p.player.loopEndSec() } },
                  { "loudness", loudness ? json (*loudness) : json (nullptr) } }
        .dump (1);
}

bool saveProject (im::InteractiveMixerProcessor& p, std::optional<double> loudness, const juce::File& f)
{
    const auto text = projectJson (p, loudness);
    auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
    return tmp.replaceWithText (text) && tmp.moveFileTo (f); // never leave a half-written project
}

std::string loadProject (im::InteractiveMixerProcessor& p, const juce::File& f, std::optional<double>& loudness)
{
    auto j = json::parse (f.loadFileAsString().toStdString(), nullptr, false);
    if (j.is_discarded() || j.value ("app", std::string()) != "mix-final-actual") return "not a mix_final_ACTUAL project";
    juce::MemoryBlock state;
    if (! state.fromBase64Encoding (juce::String (j.value ("state", std::string())))) return "damaged project file";
    const json groups = j.value ("groups", json::object()); // a named copy: items() of a temporary dangles
    for (auto& [name, role] : groups.items())
        if (role.is_number_integer()) im::priors::setRoleOverride (name, role.get<int>());
    p.setStateInformation (state.getData(), (int) state.getSize());
    if (auto l = j.value ("loop", json::array()); l.is_array() && l.size() == 2) p.player.setLoopRegion (l[0].get<double>(), l[1].get<double>());
    p.updateTrackGroups();
    if (j.contains ("loudness")) loudness = j["loudness"].is_number() ? std::optional<double> (j["loudness"].get<double>()) : std::nullopt;
    return {};
}
} // namespace mfa
