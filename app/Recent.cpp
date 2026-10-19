#include "Recent.h"
#include "core/Paths.h"
#include <nlohmann/json.hpp>

namespace mfa
{
using json = nlohmann::json;

juce::File recentFile() { return im::appDataDir().getChildFile ("recent.json"); }

std::vector<RecentProject> loadRecent (const juce::File& f)
{
    std::vector<RecentProject> out;
    auto j = json::parse (f.loadFileAsString().toStdString(), nullptr, false);
    if (! j.is_array()) return out;
    for (auto& e : j)
        if (e.is_object() && e.contains ("folder") && e["folder"].is_string())
            out.push_back ({ e["folder"].get<std::string>(), e.value ("name", std::string()), e.value ("last_opened", 0.0) });
    return out;
}

void touchRecent (const std::string& folder, const std::string& name, const juce::File& f)
{
    auto list = loadRecent (f);
    list.erase (std::remove_if (list.begin(), list.end(), [&] (const RecentProject& r) { return r.folder == folder; }), list.end());
    list.insert (list.begin(), { folder, name, (double) juce::Time::currentTimeMillis() / 1000.0 });
    if (list.size() > 20) list.resize (20);
    json j = json::array();
    for (auto& r : list) j.push_back ({ { "folder", r.folder }, { "name", r.name }, { "last_opened", r.lastOpened } });
    f.getParentDirectory().createDirectory();
    auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
    if (tmp.replaceWithText (j.dump (1))) tmp.moveFileTo (f);
}
} // namespace mfa
