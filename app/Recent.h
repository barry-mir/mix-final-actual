#pragma once
// The projects opened lately (<appdata>/recent.json, newest first, up to 20): for reopening a mix later from the page,
// the MCP tool list_projects, or `mix-final-actual open`.
#include <juce_core/juce_core.h>
#include <string>
#include <vector>

namespace mfa
{
struct RecentProject
{
    std::string folder, name;
    double lastOpened = 0; // seconds since 1970
};

juce::File recentFile();
std::vector<RecentProject> loadRecent (const juce::File& f = recentFile());
// Puts the folder first (updating its name and time), keeps 20.
void touchRecent (const std::string& folder, const std::string& name, const juce::File& f = recentFile());
} // namespace mfa
