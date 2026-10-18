#pragma once
#include <map>
#include <string>
// Mix priors: what finished mixes look like on the console's own indicators, as statistics over 230 professional mixes.
//  * per-role level of a track relative to the full mix (track energy - mix energy, dB): median, p25, p75, p10, p90
//  * per-role crest factor, stereo width and low-end share; 8-band tone profile per role and of the mix
#include <array>
#include <string>

namespace im::priors
{
struct RoleLevel
{
    const char* role;
    float median, p25, p75, p10, p90;
};
struct RoleTone
{
    std::array<float, 8> median, halfIqr;
};
struct RoleStats
{
    float cfP10, cfP50, cfP90, swP10, swP50, swP90, subP90, lowP90;
};

// GENERATED-BEGIN (statistics over 230 professional mixes)
inline constexpr int kPriorSongs = 230;
inline constexpr std::array<RoleLevel, 5> kRoleLevels { {
    { "vocals", -6.0f, -7.3f, -4.7f, -8.5f, -3.8f },
    { "drums", -6.1f, -7.4f, -4.8f, -8.9f, -3.9f },
    { "bass", -6.2f, -7.7f, -5.0f, -8.9f, -4.1f },
    { "guitars", -8.5f, -10.2f, -6.7f, -12.3f, -5.7f },
    { "keys/other", -10.6f, -13.2f, -8.4f, -17.4f, -6.7f },
} };
inline constexpr std::array<float, 8> kMixBandsMedian { -17.9f, -6.5f, -11.4f, -8.5f, -9.6f, -11.2f, -7.9f, -10.9f };
inline constexpr std::array<float, 8> kMixBandsSpread { 1.5f, 1.5f, 1.6f, 1.6f, 1.7f, 1.5f, 1.8f, 3.2f };
// whole-mix crest factor (dB) and stereo width: p10, p25, p50, p75, p90
inline constexpr std::array<float, 5> kMixCf { 13.9f, 14.3f, 15.2f, 16.1f, 17.0f };
inline constexpr std::array<float, 5> kMixSw { 0.04f, 0.06f, 0.10f, 0.15f, 0.18f };
// whole-excerpt crest (highest peak over the energy-average level, dB): p10, p25, p50, p75, p90
inline constexpr std::array<float, 5> kMixLtCf { 15.5f, 16.3f, 17.1f, 18.4f, 19.4f };
// cf p10/p50/p90 (dB), sw p10/p50/p90, sub p90, low p90 (dB rel. track total)
inline constexpr std::array<RoleStats, 5> kRoleStats { {
    { 12.2f, 14.7f, 17.5f, 0.00f, 0.05f, 0.26f, -32.8f, -11.0f }, // vocals
    { 16.3f, 19.2f, 21.9f, 0.01f, 0.04f, 0.11f, -13.1f, -2.5f }, // drums
    { 8.7f, 11.1f, 13.5f, 0.00f, 0.00f, 0.00f, -10.6f, -0.5f }, // bass
    { 12.1f, 14.3f, 17.2f, 0.08f, 0.32f, 1.05f, -19.2f, -4.6f }, // guitars
    { 10.9f, 14.0f, 16.1f, 0.11f, 0.47f, 1.02f, -18.7f, -5.1f }, // keys/other
} };
inline constexpr std::array<RoleTone, 5> kRoleTone { {
    { { -43.6f, -16.9f, -10.8f, -5.5f, -8.3f, -11.2f, -7.5f, -14.7f }, { 5.4f, 3.6f, 2.5f, 1.9f, 1.6f, 2.0f, 2.6f, 3.5f } }, // vocals
    { { -16.3f, -5.3f, -15.3f, -15.7f, -12.4f, -10.4f, -6.1f, -8.4f }, { 2.1f, 1.8f, 1.6f, 2.1f, 1.9f, 1.5f, 1.5f, 3.5f } }, // drums
    { { -11.2f, -0.7f, -13.8f, -19.4f, -28.2f, -40.4f, -48.1f, -50.6f }, { 1.5f, 1.5f, 2.7f, 6.0f, 8.4f, 6.9f, 6.6f, 5.4f } }, // bass
    { { -24.6f, -9.2f, -9.5f, -7.4f, -6.7f, -12.2f, -14.2f, -24.1f }, { 3.4f, 2.9f, 2.4f, 2.3f, 2.9f, 2.7f, 6.8f, 14.9f } }, // guitars
    { { -27.4f, -10.6f, -6.8f, -4.6f, -9.3f, -16.0f, -19.8f, -32.3f }, { 9.5f, 4.9f, 2.5f, 1.5f, 3.9f, 6.7f, 9.5f, 8.3f } }, // keys/other
} };
// GENERATED-END

// A track is flagged TOO QUIET/LOUD only outside [p25 - margin, p75 + margin] (tuned on the eval sets;
// env IM_FLAG_MARGIN overrides for experiments).
float flagMargin();
// Where a flagged track is moved: nearest edge of the typical range (default) or the median (IM_FLAG_TARGET=median).
float targetLevel (const RoleLevel& r, float level);

// Role from a track name by keywords; -1 if unknown. A group set by the user for that name wins.
int roleForName (const std::string& name);
// The user's group for a track name (0 vocals .. 4 keys/other, -1 none); kNoOverride clears it. Thread-safe.
inline constexpr int kNoOverride = -2;
void setRoleOverride (const std::string& name, int role);
std::map<std::string, int> roleOverrides();
} // namespace im::priors
