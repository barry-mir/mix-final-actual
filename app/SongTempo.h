#pragma once
// Detects the session's tempo in the background (the drum tracks around the excerpt, else every track) and stores it
// in AppState::tempo, unless the user has set the tempo by then.
namespace im
{
class InteractiveMixerProcessor;
}

namespace mfa
{
struct AppState;
void startTempoDetection (AppState& app, im::InteractiveMixerProcessor& live);
} // namespace mfa
