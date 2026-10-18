// Small native checks of the parts that do not need audio hardware or a coding agent. Run in CI on every platform:
//   mfa-tests   (exit code 0 = all passed)
#include "agent/Bridge.h"
#include "agent/MixPriors.h"
#include "agent/Tools.h"
#include "core/Params.h"
#include <juce_core/juce_core.h>
#include <iostream>
#include <thread>

namespace
{
int failures = 0;
void check (bool ok, const char* what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
    if (! ok) ++failures;
}

nlohmann::json request()
{
    using nlohmann::json;
    return { { "messages", json::array ({ { { "role", "system" }, { "content", "sys" } }, { { "role", "user" }, { "content", "measurements" } } }) },
             { "tools", json::array ({ { { "type", "function" }, { "function", { { "name", "set_fader" } } } },
                                       { { "type", "function" }, { "function", { { "name", "no_change" } } } } }) } };
}

void bridgeTests()
{
    using nlohmann::json;
    im::Bridge b;
    check (! b.waitTurn (0.05).has_value(), "bridge: no turn before one is asked");

    im::Bridge::Result res;
    std::thread asker ([&] { res = b.ask (request(), "first_mix", "", 10.0); });
    auto t = b.waitTurn (2.0);
    check (t.has_value() && t->trigger == "first_mix", "bridge: the agent gets the waiting turn");
    check (t && t->text.find ("=== SYSTEM ===") != std::string::npos && t->text.find ("set_fader") != std::string::npos,
           "bridge: the turn's text has the system part and the tools");
    auto again = b.waitTurn (0.1);
    check (again && t && again->id == t->id, "bridge: an unanswered turn is offered again");
    check (! b.answer (t->id + 1, { { "content", "x" }, { "tool_calls", json::array() } }).empty(), "bridge: a wrong turn id is refused");
    check (! b.answer (t->id, { { "content", "x" }, { "tool_calls", json::array ({ { { "name", "set_nothing" } } }) } }).empty(),
           "bridge: an unknown tool is refused");
    check (! b.answer (t->id, { { "content", "x" }, { "tool_calls", json::array ({ { { "name", "set_fader" }, { "arguments", 3 } } }) } }).empty(),
           "bridge: arguments must be an object");
    const auto err = b.answer (t->id, { { "content", "louder bass" },
                                        { "tool_calls", json::array ({ { { "name", "set_fader" }, { "arguments", { { "track", "Bass" }, { "db", 2 } } } } }) } });
    check (err.empty(), "bridge: a valid answer is accepted");
    asker.join();
    check (res.ok, "bridge: the asking side gets the answer");
    const auto parsed = im::parseChatResponse (res.response);
    check (parsed.parseError.empty() && parsed.calls.size() == 1 && parsed.calls[0].name == "set_fader" && parsed.content == "louder bass",
           "bridge: the answer parses as a chat completion with the tool call");

    std::thread asker2 ([&] { res = b.ask (request(), "user", "hi", 10.0); });
    b.waitTurn (2.0);
    b.abortPending();
    asker2.join();
    check (! res.ok, "bridge: abortPending releases the waiting turn");
    std::thread asker3 ([&] { res = b.ask (request(), "user", "", 0.2); });
    asker3.join();
    check (! res.ok && res.error.find ("no answer") != std::string::npos, "bridge: a turn times out without an agent");
    b.cancel();
    res = b.ask (request(), "user", "", 10.0);
    check (! res.ok && res.error == "cancelled", "bridge: after cancel, asks return at once");
}

void paramTests()
{
    im::setMaxTracks (150); // more than the old 64-track cap
    check (im::maxTracks() == 150, "params: the track count is set per session");
    check ((int) im::allParams().size() == im::numParams(), "params: the registry has every parameter");
    check (im::findParam ("t150.fader.gain") == im::trackParam (149, im::Fader), "params: the last track's parameters exist");
    check (im::findParam ("master.limiter.ceiling") == im::masterParam (im::LimCeiling), "params: master parameters follow the tracks");
    im::setMaxTracks (8);
    check (im::maxTracks() == 150, "params: the count cannot change once the registry exists");
    const im::TrackMask m { 3 };
    check (m.has (0) && m.has (2) && ! m.has (3) && ! m.has (-1) && m.count() == 3, "params: present tracks are 0..n-1");
}

void roleTests()
{
    using im::priors::roleForName;
    check (roleForName ("Lead Vocal") == 0 && roleForName ("Kick In") == 1 && roleForName ("Bass DI") == 2 && roleForName ("Gtr L") == 3
               && roleForName ("Piano") == 4,
           "groups: guessed from track names");
    check (roleForName ("Harmonica-Amp-CU29") == 4 && roleForName ("Tenor Sax") == 4 && roleForName ("Banjo") == 3
               && roleForName ("Tambourine") == 1 && roleForName ("Cello") == 4,
           "groups: common instruments outside the basic names");
    im::priors::setRoleOverride ("Piano", 0);
    check (roleForName ("Piano") == 0, "groups: a group set by the user wins");
    im::priors::setRoleOverride ("Piano", im::priors::kNoOverride);
    check (roleForName ("Piano") == 4, "groups: clearing it goes back to the name");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    paramTests();
    bridgeTests();
    roleTests();
    std::cout << (failures ? "FAILED: " : "all passed") << (failures ? std::to_string (failures) : std::string()) << "\n";
    return failures ? 1 : 0;
}
