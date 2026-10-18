#include "Project.h"
#include "PluginProcessor.h"
#include "agent/MixPriors.h"
#include <nlohmann/json.hpp>

namespace mfa
{
using json = nlohmann::json;

juce::File projectFile (const juce::File& tracksDir) { return tracksDir.getChildFile ("mix_final_ACTUAL project.json"); }

std::string projectJson (im::InteractiveMixerProcessor& p)
{
    juce::MemoryBlock state;
    p.getStateInformation (state);
    json groups = json::object();
    for (auto& [name, role] : im::priors::roleOverrides()) groups[name] = role;
    return json { { "app", "mix-final-actual" }, { "version", 1 }, { "state", state.toBase64Encoding().toStdString() },
                  { "groups", groups }, { "loop", { p.player.loopStartSec(), p.player.loopEndSec() } } }
        .dump (1);
}

bool saveProject (im::InteractiveMixerProcessor& p, const juce::File& f)
{
    const auto text = projectJson (p);
    auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
    return tmp.replaceWithText (text) && tmp.moveFileTo (f); // never leave a half-written project
}

std::string loadProject (im::InteractiveMixerProcessor& p, const juce::File& f)
{
    auto j = json::parse (f.loadFileAsString().toStdString(), nullptr, false);
    if (j.is_discarded() || j.value ("app", std::string()) != "mix-final-actual") return "not a mix_final_ACTUAL project";
    juce::MemoryBlock state;
    if (! state.fromBase64Encoding (juce::String (j.value ("state", std::string())))) return "damaged project file";
    for (auto& [name, role] : j.value ("groups", json::object()).items())
        if (role.is_number_integer()) im::priors::setRoleOverride (name, role.get<int>());
    p.setStateInformation (state.getData(), (int) state.getSize());
    if (auto l = j.value ("loop", json::array()); l.is_array() && l.size() == 2) p.player.setLoopRegion (l[0].get<double>(), l[1].get<double>());
    p.updateTrackGroups();
    return {};
}
} // namespace mfa
