#include "Project.h"
#include "PluginProcessor.h"
#include "agent/MixPriors.h"

namespace mfa
{
using json = nlohmann::json;

juce::File projectFile (const juce::File& tracksDir) { return tracksDir.getChildFile ("mix_final_ACTUAL project.json"); }

namespace
{
void readExtras (const json& j, ProjectExtras& x)
{
    if (j.contains ("loudness_gain_db") && j["loudness_gain_db"].is_number()) x.loudnessGainDb = j["loudness_gain_db"].get<double>();
    if (j.contains ("loudness")) x.loudness = j["loudness"].is_number() ? std::optional<double> (j["loudness"].get<double>()) : std::nullopt;
    if (j.contains ("name") && j["name"].is_string()) x.name = j["name"].get<std::string>();
    if (j.contains ("tempo") && j["tempo"].is_object()) x.tempo = j["tempo"];
    x.analysisWhole = j.value ("analysis_whole", x.analysisWhole);
}
} // namespace

std::string projectJson (im::InteractiveMixerProcessor& p, const ProjectExtras& x)
{
    juce::MemoryBlock state;
    p.getStateInformation (state);
    json groups = json::object();
    for (auto& [name, role] : im::priors::roleOverrides()) groups[name] = role;
    return json { { "app", "mix-final-actual" }, { "version", 1 }, { "state", state.toBase64Encoding().toStdString() },
                  { "groups", groups }, { "loop", { p.player.loopStartSec(), p.player.loopEndSec() } },
                  { "loudness", x.loudness ? json (*x.loudness) : json (nullptr) },
                  { "loudness_gain_db", std::round (x.loudnessGainDb * 100.0) / 100.0 },
                  { "name", x.name }, { "tempo", x.tempo }, { "analysis_whole", x.analysisWhole } }
        .dump (1);
}

bool saveProject (im::InteractiveMixerProcessor& p, const ProjectExtras& x, const juce::File& f)
{
    const auto text = projectJson (p, x);
    auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
    return tmp.replaceWithText (text) && tmp.moveFileTo (f); // never leave a half-written project
}

std::string loadProject (im::InteractiveMixerProcessor& p, const juce::File& f, ProjectExtras& x)
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
    readExtras (j, x);
    return {};
}

std::optional<ProjectExtras> readProjectExtras (const juce::File& f)
{
    auto j = json::parse (f.loadFileAsString().toStdString(), nullptr, false);
    if (j.is_discarded() || j.value ("app", std::string()) != "mix-final-actual") return std::nullopt;
    ProjectExtras x;
    readExtras (j, x);
    return x;
}
} // namespace mfa
