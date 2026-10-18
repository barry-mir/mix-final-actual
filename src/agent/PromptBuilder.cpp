#include "agent/PromptBuilder.h"
#include "agent/MixPriors.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace im
{
namespace
{
std::string f1 (double v, int dec = 1)
{
    if (! std::isfinite (v)) return "-inf";
    char b[32];
    std::snprintf (b, sizeof (b), "%.*f", dec, v);
    std::string s = b;
    if (s == "-0.0" || s == "-0" || s == "-0.00") s = s.substr (1);
    return s;
}

double r1 (double v) { return std::round (v * 10.0) / 10.0; }
double r2 (double v) { return std::round (v * 100.0) / 100.0; }

std::string trackLabel (int t, const std::array<std::string, kMaxTracks>& names)
{
    return std::to_string (t + 1) + " " + names[(size_t) t];
}

int count (const PromptInputs& in, const std::string& s)
{
    if (in.countTokens)
    {
        const int n = in.countTokens (s);
        if (n >= 0) return n;
    }
    return estimateTokens (s);
}
} // namespace

const std::string& systemPromptV2()
{
    static const std::string s =
        "You are an experienced mixing engineer running a console plugin (up to " + std::to_string (kMaxTracks)
        + " stereo tracks into one stereo mix). Each turn you see measurements of every track and the mix, diagnoses that compare them "
          "with typical finished mixes, your recent turns with their measured effect, the current settings, and what triggered this turn. "
          "Your job: make the mix sound like a finished, balanced production, and follow the user's requests.\n"
          "\nHow to read the data:\n"
          "- All levels in dB. level = the track's loudness relative to the whole mix (more negative = quieter in the mix). "
          "typical = the usual range for that role in finished mixes. fdr = current fader (0 = unity, -90 = off).\n"
          "- Tools set ABSOLUTE values: to raise a track by X dB, set_fader to fdr + X. Track = its index number.\n"
          "- tone bands (dB relative to the track's total): sub <60, low 60-250, lmid 250-500, mid 500-2k, umid 2-4k, pres 4-6k, bril 6-12k, air >12k Hz. "
          "cf = crest factor (peakiness), sw = stereo width, gr = compressor/gate gain reduction.\n"
          "- Stage: x -1..1 = audience left..right, y 0..1 = front..back. Use it for balance/pan, depth (reverb send, a little level), "
          "and brightness; you decide the mapping.\n"
          "\nHow to act:\n"
          "1. Balance first. For every track marked TOO QUIET or TOO LOUD, call set_fader with the suggested value in this same turn "
          "(one call per track; moves up to 10 dB are fine when a track is far off). Tracks marked ok: leave the fader unless the user asks.\n"
          "2. Tone only when NO track is marked TOO QUIET or TOO LOUD (balance changes the tone; fix it first). For a track whose tone note says "
          "a band is too strong or too weak, call set_eq_band on THAT track with the band and frequency given in brackets and a gain of about "
          "the deviation minus 1 dB (cut when too strong, boost when too weak; at most 6 dB; q 0.7-1 for bells). If only the MIX line flags a band, "
          "use 2-3 dB on ONE track that usually owns that band. At most 2 EQ bands per track per turn; check the effect next turn. EQ bands are only lf, lmf, hmf, hf. Use set_filter hpf (60-120 Hz) on non-bass tracks with sub/low energy they do not need.\n"
          "3. Dynamics: a vocal or bass with cf above 16 dB benefits from set_compressor (ratio 2-4, threshold so gr is 3-6 dB).\n"
          "4. User requests always win over typical ranges. Never touch LOCKED params. Only mute when asked.\n"
          "5. If nothing is marked and the user asked nothing, call no_change. Do not keep adjusting a mix that is already within typical ranges.\n"
          "Reply with 1-3 sentences saying what you change and why, then make ALL the tool calls for this turn.";
    return s;
}

const std::string& systemPromptV3()
{
    // Numbers come from MixPriors.h (statistics of professional mixes on the console's indicators).
    static const std::string s = [] {
        using namespace priors;
        auto lv = [] (int r) { const auto& l = kRoleLevels[(size_t) r]; return f1 (l.p25) + ".." + f1 (l.p75); };
        auto st = [] (int r) { return kRoleStats[(size_t) r]; };
        std::string o =
            "You are the mixing engineer of a console plugin (up to " + std::to_string (kMaxTracks)
            + " stereo tracks into one stereo mix). Each turn you get measurements of every track, a CHECKLIST that the plugin computed by "
              "comparing every number with " + std::to_string (kPriorSongs) + " professionally finished mixes, your recent turns with their measured effect, "
              "the current settings, and what triggered this turn.\n"
              "\nWhat the numbers mean:\n"
              "- level: the track's loudness relative to the whole mix in dB, averaged over the last seconds. Negative is normal. "
              "Raising a fader by X raises its level by a bit less than X (the mix gets louder too); lowering one track raises every other track's level slightly.\n"
              "- cf: crest factor, peak minus average in dB. Spiky drums are high; compression lowers it.\n"
              "- sw: stereo width, side energy / centre energy. 0 = mono; about 1 = very wide. Panning a mono track hard to one side reads about 1.\n"
              "- tone bands: each band's share of the track's OWN energy in dB (0 = all of it): sub <60, low 60-250, lmid 250-500, mid 500-2k, umid 2-4k, "
              "pres 4-6k, bril 6-12k, air >12k Hz. They say nothing about how loud the track is. Cutting one band raises the others slightly.\n"
              "- fdr = current fader dB (0 = unity). Tools set ABSOLUTE values: to raise by X, set_fader to fdr + X. Track = its index number.\n"
              "- gr = compressor (c) / gate (g) gain reduction mean/max dB. pan = the track's current balance, -1 left .. 0 centre .. 1 right.\n"
              "\nWhat finished mixes look like (middle half of the songs):\n"
              "- level: vocals " + lv (0) + ", drums " + lv (1) + ", bass " + lv (2) + " (any of these three can be the loudest), guitars " + lv (3)
            + ", keys/other " + lv (4) + ".\n"
              "- cf: drums " + f1 (st (1).cfP10, 0) + "-" + f1 (st (1).cfP90, 0) + ", vocals " + f1 (st (0).cfP10, 0) + "-" + f1 (st (0).cfP90, 0) + ", bass "
            + f1 (st (2).cfP10, 0) + "-" + f1 (st (2).cfP90, 0) + ". sw: bass 0 (mono), vocals and drums below " + f1 (st (0).swP90, 2) + ", guitars and keys "
            + f1 (st (3).swP10, 1) + "-" + f1 (st (3).swP90, 1) + ".\n"
              "- Compared with raw recordings, engineers mostly set the balance with faders and cut low end (high-pass) on vocals, guitars and keys. Large EQ boosts are rare.\n"
              "\nHow to act:\n"
              "1. Do every CHECKLIST item in this turn, with the tool call it gives (you may round values). The checklist already did the comparisons; "
              "do not invent fixes for numbers it did not flag.\n"
              "2. User requests always win over the checklist and the typical ranges. Translate them into tool calls (e.g. \"vocals more upfront\" = vocal fader +2..3 dB). "
              "Never touch LOCKED params. Only mute when asked.\n"
              "3. If the checklist is empty and the user asked nothing, call no_change.\n"
              "Reply with 1-3 sentences saying what you change and why, then make ALL the tool calls for this turn.";
        return o;
    }();
    return s;
}

const std::string& systemPromptV4()
{
    // Engineer prompt: the model decides the song's style and judges tracks against each other. Typical ranges are
    // context, not targets; the code only flags hard technical problems.
    static const std::string s = [] {
        using namespace priors;
        auto lv = [] (int r) { const auto& l = kRoleLevels[(size_t) r]; return f1 (l.p10) + ".." + f1 (l.p90); };
        std::string o =
            "You are the mixing engineer of a console plugin (up to " + std::to_string (kMaxTracks)
            + " stereo tracks into one stereo mix). You make the musical decisions. Each turn you get measurements of every track and of the mix, "
              "how the tracks share each frequency band, hard technical warnings, your recent turns with their measured effect, the current settings, "
              "and what triggered this turn.\n"
              "\nHow to work:\n"
              "1. Decide what this song is and what it needs: its style and era, the focal element (usually the lead vocal or lead instrument), "
              "what drives it (groove, riff, chords), and how dense, wide, bright and punchy it should feel. On your first turn state this as "
              "\"Intent: ...\" and keep it consistent on later turns unless the user changes it or the result tells you it was wrong.\n"
              "2. Balance by relationships, not by averages: the focal element sits clearly on top of what supports it; elements that play the same role "
              "and register compete, so give each its own space (level, EQ carve, pan, depth); the low end is one decision shared by kick and bass; "
              "background parts support without masking the lead.\n"
              "3. Use the BAND SHARES to find masking: a band where two important parts both hold a large share is a conflict. Resolve it on the part "
              "that matters less in that band (a cut there, a high-pass, a pan, or less level), rather than boosting the lead.\n"
              "4. Make a few purposeful moves per turn, then judge their measured effect next turn. Prefer cuts to boosts, and small EQ moves "
              "(1-4 dB) to big ones. A move that made things worse should be undone.\n"
              "5. User requests always win. Never touch LOCKED params. Only mute when asked. If the mix already serves your intent and the user asked "
              "nothing, call no_change.\n"
              "\nWhat the numbers mean:\n"
              "- level: the track's loudness relative to the whole mix in dB, averaged over the last seconds (negative is normal). Raising a fader by X "
              "raises its level by a bit less than X; lowering one track raises every other track's level slightly.\n"
              "- cf: crest factor (peak minus average, dB): high = spiky and dynamic, low = dense or compressed. sw: stereo width (side/centre energy): "
              "0 = mono, about 1 = very wide.\n"
              "- tone bands: each band's share of the track's OWN energy in dB (0 = all of it): sub <60, low 60-250, lmid 250-500, mid 500-2k, "
              "umid 2-4k, pres 4-6k, bril 6-12k, air >12k Hz.\n"
              "- BAND SHARES: for each band, the percentage of the MIX's energy in that band that comes from each track.\n"
              "- SETTINGS lists only values that differ from the default, so a parameter you deliberately set TO its default does not appear there. "
              "That is not a failed change; do not re-send it every turn.\n"
              "- fdr = current fader dB. Tools set ABSOLUTE values: to raise by X, set_fader to fdr + X. Track = its index number. "
              "gr = compressor/gate gain reduction mean/max dB. pan = the track's current balance, -1 left .. 0 centre .. 1 right.\n"
              "\nReference, not a target - and these are for ALL tracks of that instrument combined, as shown in the GROUPS line, not for "
              "one track of it: in " + std::to_string (kPriorSongs) + " professional multitrack mixes the level of vocals was usually "
            + lv (0) + " dB, drums " + lv (1) + ", bass " + lv (2) + ", guitars " + lv (3) + ", keys/other " + lv (4)
            + " (10th to 90th percentile). Songs differ a lot; a choice outside these ranges is fine when your intent calls for it.\n"
              "\nReply with \"Intent: ...\" (first turn, or when it changes), then 1-3 sentences on what you change and why, then ALL the tool calls for this turn.";
        return o;
    }();
    return s;
}

const std::string& systemPromptV5()
{
    // Engineer prompt for many tracks (one per instrument) and the full effect set. The craft section states what
    // finished human mixes do that the v4 runs did not (measured on 40 contest mixes vs the model's): a wider image,
    // vocals placed in space with echo/reverb, steadier dynamics from compression, brighter focal elements.
    static const std::string s = [] {
        std::string o =
            "You are the mixing engineer of a console plugin (up to " + std::to_string (kMaxTracks)
            + " stereo tracks, usually one per instrument or microphone, into one stereo mix). You make every musical decision; aim for a "
              "release-quality mix that a professional would sign off, not a cautious touch-up. Each turn you get measurements of every track and of "
              "the mix, how the tracks share each frequency band, hard technical warnings, your recent turns with their measured effect, the current "
              "settings, and what triggered this turn.\n"
              "\nHow to work:\n"
              "1. Decide what this song is and what it needs: style and era, the focal element (usually the lead vocal), what drives it, and how dense, "
              "wide, bright and punchy it should feel. On your first turn state this as \"Intent: ...\" and keep it unless the result tells you it was wrong.\n"
              "2. First turn: build the whole mix in one go. Work through EVERY track: level, pan, high-pass, EQ, dynamics, sends. Group related tracks by "
              "their names (kick in/out, snare top/bottom, overhead L/R, double-tracked parts \"dt\"/\"double\"/\"2\", amp + DI, multiple mics on one "
              "source) and treat each group as one instrument. Later turns: correct, do not rebuild. Change only what the measurements "
              "and the MIX VS FINISHED MIXES line flag, a handful of moves at a time and one cause at a time, so the next turn can tell what each did; "
              "undo what made things worse. Big simultaneous corrections make the mix swing past the target and cost turns.\n"
              "3. Balance by relationships, and check the GROUPS line every turn: nine vocal tracks each sitting at a sensible level still add up to a vocal "
              "that buries the song, so judge levels by what each instrument totals, not only track by track. The focal element clearly on top, supporting parts underneath; parts in the same register get their own space "
              "(level, EQ carve, pan, depth); kick and bass share one low-end decision. Use BAND SHARES to find masking.\n"
              "4. User requests always win. Never touch LOCKED params. Only mute when asked (a silent or unused track can be pulled down with its fader).\n"
              "\nHow finished professional mixes are built (use the tools to get there; the numbers are typical, not targets):\n"
              "- Stereo image: wide. In finished mixes the whole mix's sw (side/centre energy) sits around 0.10 (middle half 0.06-0.15) with about 40% of "
              "the energy clearly off-centre; that comes from placing parts apart, not from widening everything. Keep kick, snare, "
              "bass and lead vocal centred; pan everything else: double-tracked guitars/vocals hard left and right (-1/+1, or +-0.8), stereo pairs "
              "(overheads, piano, room, synth L/R) to opposite sides, toms across the kit, backing vocals and percussion off to the sides, keys/guitar parts "
              "that compete to opposite sides. A single mono part that should be wide gets set_doubler (mix 0.5-0.9) or a partner pan, not a centre seat. "
              "Two microphones on the SAME source (amp mics, kick in/out, snare top/bottom) are nearly the same signal: panning them apart does not "
              "widen anything, it just moves the source and can cancel in mono. Use such a pair for tone, keep it together, and take width from parts "
              "that genuinely differ (doubles, stereo pairs, separate players) or from set_doubler.\n"
              "- Space and depth: the lead vocal almost always gets an echo send (set_echo times to the song's tempo, e.g. 1/8 and 3/16 note; feedback "
              "0.2-0.35; high_cut 4-6 kHz so repeats sit behind the voice; send about -20..-10 dB) and some reverb (send about -20..-12 dB). Snare, "
              "backing vocals, keys and guitars usually get reverb too; bass and kick stay dry. Set the shared reverb to the song (decay ~1.2-2.5 s, "
              "predelay 20-60 ms).\n"
              "- Dynamics: finished mixes are steady (short-term loudness varies ~4 LU) and land at a mix cf of 14-17 dB. Compress the lead vocal (ratio 3-4, attack 5-15 ms, release "
              "80-150 ms, threshold so gr is 4-8 dB), bass (3-5 dB gr), kick/snare/room as the style wants, and use the glue compressor on the mix "
              "(ratio 2, attack 10-30 ms, 1-3 dB gr). Compare cf before/after; a vocal or bass with cf above ~16 dB is not controlled yet. "
              "Two crest readings mean different things. cf (3 s windows) is density: low means squashed. whole-excerpt cf is the highest peak "
              "since your last change over the average level: it is set by a few hits (a crash, a fill, one hard snare, stacked mics landing "
              "together) standing far above the rest, and finished mixes keep it in the 16-18 dB range. Bring it down by rounding off those "
              "hits (set_drive on the track, clip_db on its group bus), not by compressing everything, which lowers the 3 s cf instead. "
              "The MIX cf is shown as two figures, after/before the glue compressor and limiter. The second is what the mix itself is: "
              "leaning on the bus only moves the first, so control peaks where they are made and keep the two close. Compressors ride level but do not lower peaks much: the first "
              "milliseconds of a hit pass the attack and makeup gain lifts them again, so a track's cf barely drops even with several dB of "
              "gain reduction. To lower cf, round off the peaks with set_drive (2-4 dB on the spiky drums and vocals). "
              "Peaks that come from several tracks hitting together (kick, snare and overhead mics; stacked vocals) are only reachable "
              "on the group bus: set_group_bus drums with clip_db 2-4 and a slow glue compressor (ratio 2-4, attack 10-30 ms, release "
              "100-300 ms, threshold from the GROUPS bus rms for 1-3 dB gr) is how finished mixes keep the kit steady and its peaks in "
              "check; vocals often get the same lighter treatment. Bus compression also steadies the song's loudness from section to "
              "section, which drive cannot do. To bring the MIX cf down, shorten the attack on the spiky tracks (snare and drums, "
              "cf above ~22) to 1-5 ms rather than pushing thresholds "
              "further down: once a compressor shows 3-6 dB gr, a lower threshold costs loudness without flattening the peaks. Set the glue compressor's "
              "threshold from the MIX lufs reading (roughly lufs + 4 dB); its default sits far above a quiet mix, where it does nothing. Your own cuts "
              "move the mix level, so re-derive that threshold whenever lufs has drifted more than about 3 dB since you set it: a stranded glue "
              "compressor is the usual reason a mix stays uneven from section to section.\n"
              "- Tone and clarity: high-pass everything that is not kick or bass (vocals 80-120 Hz, guitars/keys 80-150, overheads 150-300); cut mud "
              "(200-500 Hz) where several parts pile up; give the focal element presence and air with boosts (lead vocal +2..5 dB around 2.5-5 kHz and/or "
              "an air shelf at 10-12 kHz), and add attack where it matters (kick 3-5 kHz, snare 4-6 kHz). Boosts are normal when they serve the intent; "
              "de-ess a brightened vocal (set_deesser, freq 5-8 kHz). set_drive rounds off the top few dB of a track's peaks and adds density to bass, drums or a dull vocal. "
              "Presence and air boosts add up across tracks: lift the focal element and CUT its competitors in that band instead of boosting every track, "
              "or the mix ends up brighter and thinner than a finished one. The MIX VS FINISHED MIXES line flags tone, cf and sw that are outside what "
              "finished mixes do; treat anything it lists as something to fix this turn.\n"
              "- Do not leave a track at defaults unless that is a decision.\n"
              "\nWhat the numbers mean:\n"
              "- level: the track's loudness relative to the whole mix in dB (negative is normal). Raising a fader by X raises its level by a bit less than X.\n"
              "- dyn: the ABSOLUTE level in dBFS that this track's gate and compressor actually see. They sit before pan and fader, so dyn does not "
              "move when you change the fader. Their thresholds are absolute dBFS, so set them from dyn and never from level: a threshold near dyn with "
              "ratio 3-4 gives a few dB of gain reduction on the loud parts, and peaks reach about dyn + cf. A threshold well below dyn squashes the "
              "track and costs loudness; one above dyn + cf never engages. Check the gr column next turn, reading mean and max apart: on a "
              "sustained source (vocal, bass) aim for 3-6 dB MEAN gr; on a percussive one (cf above ~20) a correct setting shows only 1-3 dB mean with "
              "10-20 dB on individual hits, so judge drums by max gr and by the mix cf, never by mean gr. A gr of 0.0 means the threshold is too high.\n"
              "- cf: crest factor (peak minus average, dB): high = spiky and dynamic, low = dense or compressed. sw: stereo width (side/centre energy): "
              "0 = mono, about 1 = very wide.\n"
              "- tone bands: each band's share of the track's OWN energy in dB (0 = all of it): sub <60, low 60-250, lmid 250-500, mid 500-2k, "
              "umid 2-4k, pres 4-6k, bril 6-12k, air >12k Hz. Because it is a share, a high-pass lowers the band AND "
              "the track's total, so the number moves less than the cut you made; judge it by whether the warning clears, not by the size of "
              "the change.\n"
              "- BAND SHARES: for each band, the percentage of the MIX's energy in that band that comes from each track.\n"
              "- fdr = current fader dB. Tools set ABSOLUTE values: to raise by X, set_fader to fdr + X. Track = its index number. "
              "gr = compressor/gate gain reduction mean/max dB. pan = the track's current balance, -1 left .. 0 centre .. 1 right.\n"
              "\nReply with \"Intent: ...\" (first turn, or when it changes), then a few sentences on what you change and why, then ALL the tool calls "
              "for this turn (as many as the mix needs).";
        if (paperDynamics()) // the paper's wording: no whole-excerpt crest, no group buses, drive as saturation
            for (auto [from, to] : std::initializer_list<std::pair<const char*, const char*>> {
                     { "Two crest readings mean different things. cf (3 s windows) is density: low means squashed. whole-excerpt cf is the highest peak "
                       "since your last change over the average level: it is set by a few hits (a crash, a fill, one hard snare, stacked mics landing "
                       "together) standing far above the rest, and finished mixes keep it in the 16-18 dB range. Bring it down by rounding off those "
                       "hits (set_drive on the track, clip_db on its group bus), not by compressing everything, which lowers the 3 s cf instead. ", "" },
                     { "Compressors ride level but do not lower peaks much: the first "
                       "milliseconds of a hit pass the attack and makeup gain lifts them again, so a track's cf barely drops even with several dB of "
                       "gain reduction. To lower cf, round off the peaks with set_drive (2-4 dB on the spiky drums and vocals). "
                       "Peaks that come from several tracks hitting together (kick, snare and overhead mics; stacked vocals) are only reachable "
                       "on the group bus: set_group_bus drums with clip_db 2-4 and a slow glue compressor (ratio 2-4, attack 10-30 ms, release "
                       "100-300 ms, threshold from the GROUPS bus rms for 1-3 dB gr) is how finished mixes keep the kit steady and its peaks in "
                       "check; vocals often get the same lighter treatment. Bus compression also steadies the song's loudness from section to "
                       "section, which drive cannot do. ", "" },
                     { "set_drive rounds off the top few dB of a track's peaks and adds density to bass, drums or a dull vocal.",
                       "set_drive (2-8 dB) adds density and harmonics to bass, drums or a dull vocal." } })
            {
                const auto i = o.find (from);
                jassert (i != std::string::npos);
                if (i != std::string::npos) o.replace (i, std::strlen (from), to);
            }
        return o;
    }();
    return s;
}

const std::string& systemPrompt (int version)
{
    if (version >= 5) return systemPromptV5();
    if (version >= 4) return systemPromptV4();
    if (version >= 3) return systemPromptV3();
    if (version >= 2) return systemPromptV2();
    static const std::string s =
        "You are a live mixing engineer running a multitrack console plugin (up to " + std::to_string (kMaxTracks)
        + " stereo tracks into one stereo mix). "
        "Each turn you get the tracks with their stage position and current audio features, your recent turns with their measured effect, "
        "the non-default settings and what triggered this turn. Improve the mix gradually.\n"
        "\nConventions:\n"
        "- Levels dB (meters dBFS), frequencies Hz, times ms, reverb decay s. -90 dB = -inf.\n"
        "- Tool \"track\" = the index number (e.g. \"4\") or the exact name (e.g. \"Vocal\").\n"
        "- Tools set ABSOLUTE target values. fdr = current fader dB (0 = unity); a fader is not a meter: to make a track 2 dB louder, "
        "set_fader to its fdr + 2.\n"
        "- TRACKS columns (3 s window): fdr = fader dB; in/out = RMS dBFS before/after the strip (out includes fader); pk = peak dBFS; cf = crest factor dB; "
        "sw = stereo width (side/mid energy, 0 = mono); si = stereo imbalance (-1 all left, +1 all right); "
        "8 bands = dB relative to the track total: sub <60, low 60-250, lmid 250-500, mid 500-2k, umid 2-4k, pres 4-6k, bril 6-12k, air >12k Hz; "
        "gr = c(omp)/g(ate) gain reduction dB mean/max.\n"
        "- Stage: top-down, audience at the bottom. pos x,y: x -1..1 = audience left..right, y 0..1 = front edge..back wall. "
        "You decide how position maps to the mix (balance, level, reverb send, high frequencies); nothing is mapped automatically.\n"
        "- SETTINGS lists only non-default values as module(param=value).\n"
        "\nRules:\n"
        "- Change little per turn (typically <= 3 dB on levels and EQ); several turns refine the mix.\n"
        "- Prefer no_change when the mix is balanced or a recent change has not settled yet.\n"
        "- User instructions override your own preferences.\n"
        "- Never touch LOCKED params. Only mute when the user asked.\n"
        "- Output a one-to-three sentence rationale, then the tool calls (no_change(reason) to make no change).";
    return s;
}

int estimateTokens (const std::string& s)
{
    int n = 0;
    size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = (unsigned char) s[i];
        if (std::isdigit (c)) { ++n; ++i; }
        else if (std::isalpha (c))
        {
            size_t j = i;
            while (j < s.size() && std::isalpha ((unsigned char) s[j])) ++j;
            n += (int) ((j - i + 4) / 5);
            i = j;
        }
        else if (std::isspace (c))
        {
            size_t j = i;
            while (j < s.size() && std::isspace ((unsigned char) s[j])) ++j;
            if (! (j - i == 1 && c == ' ')) ++n;
            i = j;
        }
        else { ++n; ++i; }
    }
    return n;
}

int fixedPrefixEstimate (int version)
{
    if (version >= 5)
    {
        static const int n5 = (int) std::ceil (estimateTokens (systemPrompt (5)) * 0.86)
                              + (int) std::ceil (estimateTokens (toolSchemas().dump()) * 0.66) + kTemplateOverheadTokens;
        return n5;
    }
    if (version >= 4)
    {
        static const int n4 = (int) std::ceil (estimateTokens (systemPrompt (4)) * 0.86)
                              + (int) std::ceil (estimateTokens (toolSchemas().dump()) * 0.66) + kTemplateOverheadTokens;
        return n4;
    }
    if (version >= 3)
    {
        static const int n3 = (int) std::ceil (estimateTokens (systemPrompt (3)) * 0.86)
                              + (int) std::ceil (estimateTokens (toolSchemas().dump()) * 0.66) + kTemplateOverheadTokens;
        return n3;
    }
    if (version >= 2)
    {
        static const int n2 = (int) std::ceil (estimateTokens (systemPrompt (2)) * 0.86)
                              + (int) std::ceil (estimateTokens (toolSchemas().dump()) * 0.66) + kTemplateOverheadTokens;
        return n2;
    }
    // Calibrated against the Qwen3.5 tokenizer (llama-server /tokenize): prose ~0.86x and compact JSON ~0.66x of
    // the structural estimate (system 510 / tools 1584 real tokens). Used only when the server is offline.
    static const int n = (int) std::ceil (estimateTokens (systemPrompt (1)) * 0.86)
                         + (int) std::ceil (estimateTokens (toolSchemas().dump()) * 0.66) + kTemplateOverheadTokens;
    return n;
}

std::vector<int> tracksTouched (const std::vector<AppliedChange>& changes)
{
    std::set<int> t;
    for (auto& c : changes)
    {
        const int i = findParam (c.param);
        if (i >= 0 && spec (i).track >= 0 && c.status != "rejected") t.insert (spec (i).track);
    }
    return { t.begin(), t.end() };
}

json compactFeatures (const Snapshot* s, const std::vector<int>& tracks)
{
    if (s == nullptr || s->seq == 0) return nullptr;
    auto bands = [] (const StereoFeatures& f) {
        json b = json::array();
        for (auto v : f.bands) b.push_back (std::round (v));
        return b;
    };
    json j;
    j["master"] = { { "rms", r1 (s->master.f.rmsDb) }, { "lufs", r1 (s->master.lufs) }, { "peak", r1 (s->master.peakDb) },
                    { "sw", r2 (s->master.f.sw) }, { "si", r2 (s->master.f.si) }, { "bands", bands (s->master.f) } };
    json tr = json::object();
    for (int t : tracks)
    {
        const auto& ts = s->tracks[(size_t) t];
        if (! ts.present) continue;
        if (ts.silent) { tr[std::to_string (t + 1)] = "silent"; continue; }
        tr[std::to_string (t + 1)] = { { "rms", r1 (ts.post.rmsDb) }, { "peak", r1 (ts.peakDb) }, { "sw", r2 (ts.post.sw) },
                                       { "si", r2 (ts.post.si) }, { "bands", bands (ts.post) }, { "comp_gr", r1 (ts.compGrMean) } };
    }
    j["tracks"] = tr;
    return j;
}

std::string tracksSection (const PromptInputs& in)
{
    const auto* s = in.snapshot.get();
    std::ostringstream o;
    const bool haveFeat = s != nullptr && s->seq != 0;
    o << "TRACKS\n# name pos fdr | in out pk cf sw si | sub low lmid mid umid pres bril air | gr\n";
    for (int t = 0; t < kMaxTracks; ++t)
    {
        if (! ((in.presentMask >> t) & 1u)) continue;
        const auto& p = in.stage[(size_t) t];
        o << (t + 1) << " " << in.names[(size_t) t] << " " << f1 (p.x, 2) << "," << f1 (p.y, 2) << " "
          << (in.paramValue ? formatValue (spec (trackParam (t, Fader)), in.paramValue (trackParam (t, Fader)), false) : std::string ("0.0")) << " | ";
        if (! haveFeat) { o << "(no features yet)\n"; continue; }
        const auto& ts = s->tracks[(size_t) t];
        if (ts.silent || ! ts.present) { o << "silent\n"; continue; }
        o << f1 (ts.pre.rmsDb) << " " << f1 (ts.post.rmsDb) << " " << f1 (ts.peakDb) << " " << f1 (ts.post.cfDb) << " "
          << f1 (ts.post.sw, 2) << " " << f1 (ts.post.si, 2) << " |";
        for (auto b : ts.post.bands) o << " " << f1 (b, 0);
        o << " |";
        const bool compOn = in.paramValue && in.paramValue (trackParam (t, CompOn)) > 0.5f;
        const bool gateOn = in.paramValue && in.paramValue (trackParam (t, GateOn)) > 0.5f;
        if (compOn) o << " c" << f1 (ts.compGrMean) << "/" << f1 (ts.compGrMax);
        if (gateOn) o << " g" << f1 (ts.gateGrMean) << "/" << f1 (ts.gateGrMax);
        if (! compOn && ! gateOn) o << " -";
        o << "\n";
    }
    if (haveFeat)
    {
        const auto& m = s->master;
        o << "MASTER lufs " << f1 (m.lufs) << " rms " << f1 (m.f.rmsDb) << " pk " << f1 (m.peakDb) << " cf " << f1 (m.f.cfDb)
          << " sw " << f1 (m.f.sw, 2) << " si " << f1 (m.f.si, 2) << " |";
        for (auto b : m.f.bands) o << " " << f1 (b, 0);
        o << "\n";
    }
    return o.str();
}

namespace priors
{
float flagMargin()
{
    static const float m = [] {
        if (const char* e = std::getenv ("IM_FLAG_MARGIN")) return (float) std::atof (e);
        return 0.0f; // tuned: margin 0 + move to the range edge was best across both eval sets
    }();
    return m;
}

float targetLevel (const RoleLevel& r, float level)
{
    static const bool median = [] { const char* e = std::getenv ("IM_FLAG_TARGET"); return e != nullptr && std::string (e) == "median"; }();
    if (median) return r.median;
    return level < r.p25 ? r.p25 : level > r.p75 ? r.p75 : level; // nearest edge of the typical range
}

int roleForName (const std::string& name)
{
    std::string n;
    for (char c : name.substr (0, name.find (" ("))) n += (char) std::tolower ((unsigned char) c); // label before any "(contents)"
    auto has = [&] (std::initializer_list<const char*> ks) {
        for (auto* k : ks)
            if (n.find (k) != std::string::npos) return true;
        return false;
    };
    if (has ({ "vocal", "vox", "voice", "sing", "lead v", "bv", "choir", "rap" })) return 0;
    if (has ({ "drum", "kick", "snare", "hat", "tom", "overhead", "oh ", "cymbal", "perc", "beat" })) return 1;
    if (has ({ "bass", "sub" })) return 2;
    if (has ({ "guitar", "gtr", "git" })) return 3;
    if (has ({ "key", "piano", "synth", "organ", "pad", "string", "brass", "horn", "other", "wind", "fx" })) return 4;
    return -1;
}
} // namespace priors

float levelVsMix (const Snapshot& s, int t)
{
    const auto& ts = s.tracks[(size_t) t];
    if (s.avgSec >= 6.0 && ts.levelAvgDb > -100.0f) return ts.levelAvgDb;
    return ts.post.rmsDb - s.master.f.rmsDb;
}

std::vector<ToneIssue> trackToneIssues (const StereoFeatures& post, int role)
{
    static const int slot[kAggBands] = { LfGain, LfGain, LmfGain, LmfGain, HmfGain, HmfGain, HfGain, HfGain };
    static const float freq[kAggBands] = { 60, 120, 350, 1000, 3000, 5000, 9000, 14000 };
    std::vector<std::pair<float, ToneIssue>> dev;
    // keys/other mixes piano, synths, strings and winds: no meaningful tonal prior
    if (role < 0 || role == 4) return {};
    const auto& rt = priors::kRoleTone[(size_t) role];
    for (int g = 0; g < kAggBands; ++g)
    {
        if (rt.median[(size_t) g] < -20.0f) continue; // only bands where the role normally has substantial energy
        const float d = post.bands[(size_t) g] - rt.median[(size_t) g];
        if (d <= 0.0f) continue;                      // cut-only: "too weak" in a band is rarely fixed by boosting
        // Strict on purpose: in a tone test on professional mixes, deviations up to ~5 spreads are as common on healthy tracks
        // as on damaged ones (natural song-to-song variation), so only flag extreme cases (>6 spreads, >=5 dB).
        const float lim = std::max (5.0f, 6.0f * std::max (1.0f, rt.halfIqr[(size_t) g]));
        if (std::fabs (d) > lim) dev.push_back ({ std::fabs (d) / lim, ToneIssue { g, d, slot[g], freq[g] } });
    }
    std::sort (dev.begin(), dev.end(), [] (auto& x, auto& y) { return x.first > y.first; });
    std::vector<ToneIssue> out;
    for (size_t k = 0; k < dev.size() && k < 2; ++k) out.push_back (dev[k].second);
    return out;
}

std::string tracksSectionV2 (const PromptInputs& in)
{
    const auto* s = in.snapshot.get();
    const bool haveFeat = s != nullptr && s->seq != 0;
    std::ostringstream o;
    if (! haveFeat) return "TRACKS: no measurements yet (analysis warming up)\n";
    const auto& m = s->master;

    // MIX line with tone vs. typical finished mixes
    o << "MIX: lufs " << f1 (m.lufs) << " | peak " << f1 (m.peakDb) << " dBFS | tone vs typical finished mixes:";
    std::string tone;
    // which set_eq_band band (and centre) addresses each analysis band
    static const char* eqFor[kAggBands] = { "lf 60", "lf 120", "lmf 350", "lmf 1000", "hmf 3000", "hmf 5000", "hf 9000", "hf 14000" };
    static const char* owners[kAggBands] = { "bass/kick", "bass/drums", "guitars/keys/bass", "vocals/guitars/keys", "vocals/guitars",
                                             "vocals/drums", "drums/vocals", "drums/vocals" };
    for (int g = 0; g < kAggBands; ++g)
    {
        const float d = m.f.bands[(size_t) g] - priors::kMixBandsMedian[(size_t) g];
        // only clear problems: > max(3 dB, 2 x half-IQR) away from typical finished mixes
        if (std::fabs (d) > std::max (3.0f, 2.0f * priors::kMixBandsSpread[(size_t) g]))
            tone += std::string (" ") + kAggBandNames[g] + (d > 0 ? " too strong +" : " too weak ") + f1 (d, 1) + " (eq " + eqFor[g]
                    + " Hz; usually " + owners[g] + ");";
    }
    o << (tone.empty() ? " balanced" : tone) << "\n";

    o << "TRACKS\n# name pos fdr | level (typical) diagnosis | cf sw | sub low lmid mid umid pres bril air | gr\n";
    for (int t = 0; t < kMaxTracks; ++t)
    {
        if (! ((in.presentMask >> t) & 1u)) continue;
        const auto& p = in.stage[(size_t) t];
        const float fdr = in.paramValue ? in.paramValue (trackParam (t, Fader)) : 0.0f;
        o << (t + 1) << " " << in.names[(size_t) t] << " " << f1 (p.x, 2) << "," << f1 (p.y, 2) << " "
          << formatValue (spec (trackParam (t, Fader)), fdr, false) << " | ";
        const auto& ts = s->tracks[(size_t) t];
        if (ts.silent || ! ts.present) { o << "silent\n"; continue; }
        const float level = levelVsMix (*s, t);
        o << f1 (level);
        const int role = priors::roleForName (in.names[(size_t) t]);
        if (role >= 0)
        {
            const auto& r = priors::kRoleLevels[(size_t) role];
            o << " (" << f1 (r.p25) << ".." << f1 (r.p75) << " " << r.role << ") ";
            if (level < r.p25 - priors::flagMargin() || level > r.p75 + priors::flagMargin())
            {
                const float move = priors::targetLevel (r, level) - level;
                const float target = fdr + move;
                o << (level < r.p25 ? "TOO QUIET by " : "TOO LOUD by ") << f1 (std::fabs (move)) << " -> set_fader "
                  << f1 (juce::jlimit (-60.0f, 12.0f, target));
            }
            else
                o << "ok";
        }
        else
            o << " (role unknown)";
        // per-track tone vs the role's typical profile: at most 2 clear deviations
        if (role >= 0)
        {
            static const char* eqBand[kAggBands] = { "lf 60", "lf 120", "lmf 350", "lmf 1000", "hmf 3000", "hmf 5000", "hf 9000", "hf 14000" };
            const auto issues = trackToneIssues (ts.post, role);
            o << "; tone ";
            if (issues.empty()) o << "ok";
            for (size_t k = 0; k < issues.size(); ++k)
                o << (k ? ", " : "") << kAggBandNames[issues[k].band] << (issues[k].dev > 0 ? " too strong +" : " too weak ")
                  << f1 (issues[k].dev) << " (eq " << eqBand[issues[k].band] << " Hz)";
        }
        o << " | " << f1 (ts.post.cfDb) << " " << f1 (ts.post.sw, 2) << " |";
        for (auto b : ts.post.bands) o << " " << f1 (b, 0);
        o << " |";
        const bool compOn = in.paramValue && in.paramValue (trackParam (t, CompOn)) > 0.5f;
        const bool gateOn = in.paramValue && in.paramValue (trackParam (t, GateOn)) > 0.5f;
        if (compOn) o << " c" << f1 (ts.compGrMean) << "/" << f1 (ts.compGrMax);
        if (gateOn) o << " g" << f1 (ts.gateGrMean) << "/" << f1 (ts.gateGrMax);
        if (! compOn && ! gateOn) o << " -";
        o << "\n";
    }
    return o.str();
}

std::vector<std::string> checklistV3 (const PromptInputs& in)
{
    std::vector<std::string> balance, other;
    const auto* s = in.snapshot.get();
    if (s == nullptr || s->seq == 0) return {};
    auto pv = [&] (int t, int slot) { return in.paramValue ? in.paramValue (trackParam (t, slot)) : 0.0f; };
    for (int t = 0; t < kMaxTracks; ++t)
    {
        if (! ((in.presentMask >> t) & 1u)) continue;
        const auto& ts = s->tracks[(size_t) t];
        if (ts.silent || ! ts.present) continue;
        const int role = priors::roleForName (in.names[(size_t) t]);
        if (role < 0) continue;
        const auto who = trackLabel (t, in.names);
        const auto tn = std::to_string (t + 1);
        const auto& r = priors::kRoleLevels[(size_t) role];
        const auto& st = priors::kRoleStats[(size_t) role];
        const float level = levelVsMix (*s, t);
        const float fdr = pv (t, Fader);
        // 0.5 dB dead band and a target 0.5 dB inside the range: a track parked exactly on the edge would be re-flagged
        // every turn by measurement noise and block the tone/width/dynamics items.
        constexpr float kDead = 0.5f;
        if (level < r.p25 - kDead || level > r.p75 + kDead)
        {
            const float move = (level < r.p25 ? std::min (r.p25 + kDead, r.median) : std::max (r.p75 - kDead, r.median)) - level;
            balance.push_back (who + ": level " + f1 (level) + " is " + f1 (std::fabs (move)) + " dB " + (move > 0 ? "below" : "above")
                               + " finished " + r.role + " (" + f1 (r.p25) + ".." + f1 (r.p75) + ") -> set_fader track " + tn + " db "
                               + f1 (juce::jlimit (-60.0f, 12.0f, fdr + move)));
        }
        // low-end rumble on a non-bass track: raw recordings carry 3-8 dB more sub than finished ones
        if (role != 2 && role != 1 && ts.post.bands[0] > st.subP90 && ! (pv (t, HpfOn) > 0.5f && pv (t, HpfFreq) >= 70.0f))
            other.push_back (who + ": sub " + f1 (ts.post.bands[0], 0) + " dB, finished " + r.role + " stay below " + f1 (st.subP90, 0)
                             + " -> set_filter track " + tn + " type hpf freq " + (role == 0 ? "100" : "80") + " enabled true");
        // drums: boomy kick (sub and low both above finished drums)
        if (role == 1 && ts.post.bands[0] > st.subP90 && ts.post.bands[1] > st.lowP90)
            other.push_back (who + ": sub " + f1 (ts.post.bands[0], 0) + " / low " + f1 (ts.post.bands[1], 0) + " dB, finished drums stay below "
                             + f1 (st.subP90, 0) + " / " + f1 (st.lowP90, 0) + " -> set_eq_band track " + tn + " band lf freq 80 gain_db -3 shape shelf");
        // width: bass is mono in finished mixes; vocals and drums nearly so
        if (role <= 2 && ts.post.sw > std::max (0.05f, st.swP90) && pv (t, Width) > 0.3f)
            other.push_back (who + ": sw " + f1 (ts.post.sw, 2) + ", finished " + r.role + " stay below " + f1 (std::max (0.05f, st.swP90), 2)
                             + " -> set_width track " + tn + " value " + (role == 2 ? "0" : "0.5"));
        // dynamics: very spiky vocal or bass
        if ((role == 0 || role == 2) && ts.post.cfDb > st.cfP90 + 1.0f && ! (pv (t, CompOn) > 0.5f && ts.compGrMean > 2.0f))
            other.push_back (who + ": cf " + f1 (ts.post.cfDb) + ", finished " + r.role + " " + f1 (st.cfP10, 0) + "-" + f1 (st.cfP90, 0)
                             + " -> set_compressor track " + tn + " enabled true ratio 3 threshold_db " + f1 (std::round (ts.post.rmsDb + 6.0f), 0)
                             + " attack_ms 10 release_ms 120 makeup_db 2");
        // tone vs the role's typical profile (strict: only extreme deviations, cut only)
        static const char* eqBand[kAggBands] = { "lf", "lf", "lmf", "lmf", "hmf", "hmf", "hf", "hf" };
        static const int eqFreq[kAggBands] = { 60, 120, 350, 1000, 3000, 5000, 9000, 14000 };
        for (const auto& ti : trackToneIssues (ts.post, role))
        {
            const float slotGain = pv (t, ti.eqSlot);
            const float target = juce::jlimit (-6.0f, 6.0f, slotGain - std::min (6.0f, std::fabs (ti.dev) - 1.0f));
            other.push_back (who + ": " + kAggBandNames[ti.band] + " " + f1 (ti.dev, 0) + " dB stronger than finished " + r.role + " -> set_eq_band track " + tn
                             + " band " + eqBand[ti.band] + " freq " + std::to_string (eqFreq[ti.band]) + " gain_db " + f1 (target, 0) + " q 1 shape bell");
        }
    }
    // One turn does everything: balance and clean-up together. Clean-up moves (high-pass, shelf, compression) change
    // track levels by about a dB, which the next turns' balance items correct; doing them in separate turns cost a
    // full turn of worse balance on the FxNorm evaluation.
    if (other.size() > 4) other.resize (4);
    balance.insert (balance.end(), other.begin(), other.end());
    return balance;
}

std::string tracksSectionV3 (const PromptInputs& in)
{
    const auto* s = in.snapshot.get();
    if (s == nullptr || s->seq == 0) return "TRACKS: no measurements yet (analysis warming up)\nCHECKLIST: wait for measurements -> no_change\n";
    std::ostringstream o;
    const auto& m = s->master;
    o << "MIX: lufs " << f1 (m.lufs) << " | peak " << f1 (m.peakDb) << " dBFS | tone vs finished mixes:";
    std::string tone;
    for (int g = 0; g < kAggBands; ++g)
    {
        const float d = m.f.bands[(size_t) g] - priors::kMixBandsMedian[(size_t) g];
        if (std::fabs (d) > std::max (3.0f, 2.0f * priors::kMixBandsSpread[(size_t) g]))
            tone += std::string (" ") + kAggBandNames[g] + (d > 0 ? " +" : " ") + f1 (d, 1) + ";";
    }
    o << (tone.empty() ? " within the usual range" : tone) << "\n";
    o << "TRACKS (level averaged over " << f1 (s->avgSec >= 6.0 ? s->avgSec : s->windowSec, 0) << " s)\n"
      << "# name pan fdr | level (finished range) | cf | sw | sub low lmid mid umid pres bril air | gr\n";
    for (int t = 0; t < kMaxTracks; ++t)
    {
        if (! ((in.presentMask >> t) & 1u)) continue;
        const float fdr = in.paramValue ? in.paramValue (trackParam (t, Fader)) : 0.0f;
        const float pan = in.paramValue ? in.paramValue (trackParam (t, Balance)) : 0.0f;
        o << (t + 1) << " " << in.names[(size_t) t] << " " << f1 (pan, 2) << " " << formatValue (spec (trackParam (t, Fader)), fdr, false) << " | ";
        const auto& ts = s->tracks[(size_t) t];
        if (ts.silent || ! ts.present) { o << "silent\n"; continue; }
        o << f1 (levelVsMix (*s, t));
        const int role = priors::roleForName (in.names[(size_t) t]);
        if (role >= 0) o << " (" << f1 (priors::kRoleLevels[(size_t) role].p25) << ".." << f1 (priors::kRoleLevels[(size_t) role].p75) << ")";
        else o << " (role unknown)";
        o << " | " << f1 (ts.post.cfDb) << " | " << f1 (ts.post.sw, 2) << " |";
        for (auto b : ts.post.bands) o << " " << f1 (b, 0);
        o << " |";
        const bool compOn = in.paramValue && in.paramValue (trackParam (t, CompOn)) > 0.5f;
        const bool gateOn = in.paramValue && in.paramValue (trackParam (t, GateOn)) > 0.5f;
        if (compOn) o << " c" << f1 (ts.compGrMean) << "/" << f1 (ts.compGrMax);
        if (gateOn) o << " g" << f1 (ts.gateGrMean) << "/" << f1 (ts.gateGrMax);
        if (! compOn && ! gateOn) o << " -";
        o << "\n";
    }
    const auto items = checklistV3 (in);
    if (items.empty())
        o << "CHECKLIST: every track is inside the finished-mix ranges -> no_change (unless the user asks for something)\n";
    else
    {
        o << "CHECKLIST\n";
        for (size_t i = 0; i < items.size(); ++i) o << (items[i][0] == '(' ? "" : std::to_string (i + 1) + ". ") << items[i] << "\n";
    }
    return o.str();
}

std::string tracksSectionV4 (const PromptInputs& in)
{
    const auto* s = in.snapshot.get();
    if (s == nullptr || s->seq == 0) return "TRACKS: no measurements yet (analysis warming up)\n";
    std::ostringstream o;
    const auto& m = s->master;
    o << "MIX: lufs " << f1 (m.lufs) << " | peak " << f1 (m.peakDb) << " dBFS | cf " << f1 (m.f.cfDb) << (in.promptVersion >= 5 ? "/" + f1 (m.cfPreDb) : std::string())
      << (in.promptVersion >= 5 && ! paperDynamics() && m.ltCfPreDb > 0.0f ? " | whole-excerpt cf " + f1 (m.ltCfPreDb) : std::string()) << " | sw " << f1 (m.f.sw, 2) << " | tone";
    for (int g = 0; g < kAggBands; ++g) o << " " << kAggBandNames[g] << " " << f1 (m.f.bands[(size_t) g], 0);
    o << "\n";
    // v5: how this mix sits against the 230 finished mixes - only clear deviations, so the line is empty on a good mix
    if (in.promptVersion >= 5)
    {
        static const char* eqFor[kAggBands] = { "lf 60", "lf 120", "lmf 350", "lmf 1000", "hmf 3000", "hmf 5000", "hf 9000", "hf 14000" };
        std::string d;
        for (int g = 0; g < kAggBands; ++g)
        {
            const float dev = m.f.bands[(size_t) g] - priors::kMixBandsMedian[(size_t) g];
            if (std::fabs (dev) > std::max (2.0f, 1.5f * priors::kMixBandsSpread[(size_t) g]))
                d += std::string (" ") + kAggBandNames[g] + (dev > 0 ? " too strong +" : " too weak ") + f1 (dev) + " (eq " + eqFor[g] + " Hz);";
        }
        const auto range = [] (float lo, float hi, int dec) { return f1 (lo, dec) + "-" + f1 (hi, dec); };
        if (m.cfPreDb > priors::kMixCf[4] + 2.0f) // the prior is measured on finished (mastered) mixes, so allow headroom pre-master
            d += " cf before the bus " + f1 (m.cfPreDb) + " is well above finished mixes' " + range (priors::kMixCf[0], priors::kMixCf[4], 0)
                 + (paperDynamics() ? " (peaks stick out: compress the loudest transients, or the glue bus, harder);"
                                    : " (peaks stick out: round them off with set_drive on the spiky tracks);");
        else if (m.f.cfDb < priors::kMixCf[0])
            d += " cf " + f1 (m.f.cfDb) + " below finished " + range (priors::kMixCf[0], priors::kMixCf[4], 0) + " (over-compressed);";
        if (! paperDynamics() && m.ltCfPreDb > priors::kMixLtCf[3])
            d += " whole-excerpt cf " + f1 (m.ltCfPreDb) + " above most finished mixes (middle half " + range (priors::kMixLtCf[1], priors::kMixLtCf[3], 1)
                 + "): a few peaks stand far above the rest of the song;";
        if (m.f.sw > priors::kMixSw[4])
            d += " sw " + f1 (m.f.sw, 2) + " above finished " + range (priors::kMixSw[0], priors::kMixSw[4], 2) + " (too wide, thin centre);";
        else if (m.f.sw < priors::kMixSw[0])
            d += " sw " + f1 (m.f.sw, 2) + " below finished " + range (priors::kMixSw[0], priors::kMixSw[4], 2) + " (too narrow);";
        o << "MIX VS FINISHED MIXES:" << (d.empty() ? " nothing out of range" : d) << "\n";
    }
    const bool v5 = in.promptVersion >= 5;
    o << "TRACKS (level averaged over " << f1 (s->avgSec >= 6.0 ? s->avgSec : s->windowSec, 0) << " s)\n"
      << (v5 ? "# name pan fdr | level dyn | cf | sw | sub low lmid mid umid pres bril air | gr\n"
             : "# name pan fdr | level | cf | sw | sub low lmid mid umid pres bril air | gr\n");
    std::vector<int> live;
    for (int t = 0; t < kMaxTracks; ++t)
    {
        if (! ((in.presentMask >> t) & 1u)) continue;
        const float fdr = in.paramValue ? in.paramValue (trackParam (t, Fader)) : 0.0f;
        const float pan = in.paramValue ? in.paramValue (trackParam (t, Balance)) : 0.0f;
        o << (t + 1) << " " << in.names[(size_t) t] << " " << f1 (pan, 2) << " " << formatValue (spec (trackParam (t, Fader)), fdr, false) << " | ";
        const auto& ts = s->tracks[(size_t) t];
        if (ts.silent || ! ts.present) { o << "silent\n"; continue; }
        live.push_back (t);
        o << f1 (levelVsMix (*s, t));
        if (v5)
        {
            // the level the gate/compressor detector sees: they sit before width, pan and fader, so undo the fader and
            // the constant-power pan law (-3 dB for a mono source, whatever the pan position)
            const float fdrDb = in.paramValue ? in.paramValue (trackParam (t, Fader)) : 0.0f;
            o << " " << f1 (ts.post.rmsDb - (fdrDb > kMinusInfDb ? fdrDb : 0.0f) + 3.0f);
        }
        o << " | " << f1 (ts.post.cfDb) << " | " << f1 (ts.post.sw, 2) << " |";
        for (auto b : ts.post.bands) o << " " << f1 (b, 0);
        o << " |";
        const bool compOn = in.paramValue && in.paramValue (trackParam (t, CompOn)) > 0.5f;
        const bool gateOn = in.paramValue && in.paramValue (trackParam (t, GateOn)) > 0.5f;
        if (compOn) o << " c" << f1 (ts.compGrMean) << "/" << f1 (ts.compGrMax);
        if (gateOn) o << " g" << f1 (ts.gateGrMean) << "/" << f1 (ts.gateGrMax);
        if (! compOn && ! gateOn) o << " -";
        o << "\n";
    }
    // What the instrument groups add up to. With one track per instrument the agent sets many faders, but the listener
    // (and the comparison against finished mixes) hears the group, and small per-track errors compound across it.
    if (v5 && ! live.empty())
    {
        std::array<double, 5> e {}, eAbs {};
        std::array<int, 5> cnt {};
        for (int t : live)
        {
            const int role = priors::roleForName (in.names[(size_t) t]);
            if (role < 0) continue;
            e[(size_t) role] += std::pow (10.0, levelVsMix (*s, t) / 10.0);
            eAbs[(size_t) role] += std::pow (10.0, s->tracks[(size_t) t].post.rmsDb / 10.0); // what the group bus receives
            ++cnt[(size_t) role];
        }
        o << "GROUPS (every track of one instrument combined, level vs the whole mix - this is what a listener hears).\n"
             "  These are relative to the mix, which your own moves change: raising one group lifts the mix too, so its reading moves less than the\n"
             "  fader change and every other group's reading drops. Fix the group that is furthest outside its range first, expect to need a bigger\n"
             "  move than the gap suggests, and re-read the line next turn. A track that needs more than the fader's +12 dB can take set_trim as well.\n";
        for (int r = 0; r < 5; ++r)
        {
            if (cnt[(size_t) r] == 0) continue;
            // the tracks are metered before their group bus; what reaches the mix is after its compressor, makeup and clipper
            const double busGain = in.groupBusGain ? std::pow (10.0, in.groupBusGain (r) / 10.0) : 1.0;
            e[(size_t) r] *= busGain;
            const auto& rl = priors::kRoleLevels[(size_t) r];
            o << "  " << rl.role << " " << f1 (10.0 * std::log10 (e[(size_t) r])) << " dB from " << cnt[(size_t) r]
              << " track" << (cnt[(size_t) r] > 1 ? "s" : "") << " (finished mixes " << f1 (rl.p10) << ".." << f1 (rl.p90) << ")"
;
            if (! paperDynamics())
                o << " | bus rms " << f1 (10.0 * std::log10 (eAbs[(size_t) r] + 1e-12)) << " dBFS";
            if (in.paramValue && ! paperDynamics())
            {
                auto bv = [&] (int f) { return in.paramValue (masterParam (groupBusSlot (r, f))); };
                if (bv (GbOn) > 0.5f)
                    o << ", comp " << f1 (bv (GbRatio)) << ":1 gr " << f1 (in.groupBusGr ? in.groupBusGr (r) : 0.0f);
                else
                    o << ", comp off";
                o << ", clip " << f1 (bv (GbClip));
                if (in.groupBusGain) o << ", bus changes level " << f1 (in.groupBusGain (r)) << " dB (included in the group level)";
            }
            o << "\n";
        }
    }
    // who owns each band: track energy in the band as a share of the summed track energy in that band (same 3 s window)
    if (! live.empty())
    {
        o << "BAND SHARES (% of the mix's energy in each band; tracks under 5% omitted)\n";
        for (int g = 0; g < kAggBands; ++g)
        {
            std::vector<std::pair<double, int>> e;
            double tot = 0;
            for (int t : live)
            {
                const auto& ts = s->tracks[(size_t) t];
                const double v = std::pow (10.0, (ts.post.rmsDb + ts.post.bands[(size_t) g]) / 10.0);
                e.push_back ({ v, t });
                tot += v;
            }
            std::sort (e.begin(), e.end(), [] (auto& a, auto& b) { return a.first > b.first; });
            o << kAggBandNames[g] << ":";
            bool any = false;
            for (auto& [v, t] : e)
            {
                const double pc = tot > 0 ? 100.0 * v / tot : 0.0;
                if (pc < 5.0) continue;
                o << (any ? ", " : " ") << in.names[(size_t) t] << " " << (int) std::lround (pc) << "%";
                any = true;
            }
            o << "\n";
        }
    }
    // hard technical problems only (not taste)
    std::vector<std::string> warn;
    if (m.peakDb > -0.3f) warn.push_back ("mix peak " + f1 (m.peakDb) + " dBFS: clipping risk, lower the loudest elements or the master");
    for (int t : live)
    {
        const auto& ts = s->tracks[(size_t) t];
        const int role = priors::roleForName (in.names[(size_t) t]);
        if (role == 2 && ts.post.sw > 0.05f) warn.push_back (trackLabel (t, in.names) + ": bass has stereo width " + f1 (ts.post.sw, 2) + " (low end in the sides is usually unintended)");
        if (role >= 0 && role != 1 && role != 2 && ts.post.bands[0] > priors::kRoleStats[(size_t) role].subP90 + 3.0f)
            warn.push_back (trackLabel (t, in.names) + ": sub (<60 Hz) at " + f1 (ts.post.bands[0], 0) + " dB of its energy, well above finished " + priors::kRoleLevels[(size_t) role].role + " (rumble?)");
        if (ts.post.sw > 1.5f) warn.push_back (trackLabel (t, in.names) + ": sw " + f1 (ts.post.sw, 2) + " (more side than centre: phase problem?)");
    }
    // the glue compressor silently strands itself: the build's own cuts move the mix away from a threshold set earlier
    if (in.promptVersion >= 5 && in.paramValue && in.paramValue (masterParam (BcOn)) > 0.5f)
    {
        const float thr = in.paramValue (masterParam (BcThr));
        if (thr > m.lufs + 6.0f)
            warn.push_back ("glue compressor is on but its threshold " + f1 (thr) + " dB sits above the mix (lufs " + f1 (m.lufs)
                            + "): it is not engaging. Re-derive it from the current lufs (about lufs + 4).");
        else if (thr < m.lufs - 12.0f)
            warn.push_back ("glue compressor threshold " + f1 (thr) + " dB is far below the mix (lufs " + f1 (m.lufs)
                            + "): it is squashing everything. Raise it towards lufs + 4.");
    }
    o << "WARNINGS:" << (warn.empty() ? " none" : "") << "\n";
    for (auto& w : warn) o << "- " << w << "\n";
    return o.str();
}

std::string lockedSection (const PromptInputs& in)
{
    std::map<int, std::vector<std::string>> byTrack;
    for (int p : in.locked)
    {
        const auto& s = spec (p);
        if (s.track >= 0 && ! ((in.presentMask >> s.track) & 1u)) continue;
        byTrack[s.track].push_back (s.key);
    }
    if (byTrack.empty()) return "LOCKED: none\n";
    std::string out = "LOCKED:";
    bool first = true;
    for (auto& [t, keys] : byTrack)
    {
        out += first ? " " : "; ";
        first = false;
        out += t < 0 ? std::string ("master") : trackLabel (t, in.names);
        for (auto& k : keys) out += " " + k;
    }
    return out + "\n";
}

std::string memorySection (const std::vector<MemoryRecord>& mem, const std::array<std::string, kMaxTracks>& names)
{
    if (mem.empty()) return "MEMORY: none\n";
    std::ostringstream o;
    o << "MEMORY (your last " << mem.size() << " turns, oldest first)\n";
    for (auto& r : mem)
    {
        o << "[tick " << r.tick << " " << r.trigger;
        if (! r.userMsg.empty()) o << " \"" << r.userMsg << "\"";
        o << "] " << (r.rationale.empty() ? "(no rationale)" : r.rationale) << "\n";
        std::string ap, rj;
        for (auto& c : r.applied)
        {
            const int i = findParam (c.param);
            if (i < 0 || c.status == "superseded" || c.status == "unchanged") continue;
            const auto& s = spec (i);
            const std::string who = s.track >= 0 ? trackLabel (s.track, names) : "master";
            if (c.status == "rejected")
                rj += (rj.empty() ? "" : "; ") + who + " " + s.key + " (" + c.note + ")";
            else
                ap += (ap.empty() ? "" : "; ") + who + " " + s.key + " " + formatValue (s, c.from, false) + "->" + formatValue (s, c.to, false)
                      + (c.status != "applied" ? " [" + c.status + "]" : "");
        }
        o << " applied: " << (ap.empty() ? "none" : ap) << "\n";
        if (! rj.empty()) o << " rejected: " << rj << "\n";
        if (r.featuresBefore.is_object() && r.featuresAfter.is_object())
        {
            const auto& b = r.featuresBefore;
            const auto& a = r.featuresAfter;
            std::string e = "master rms " + f1 (b["master"].value ("rms", 0.0)) + "->" + f1 (a["master"].value ("rms", 0.0))
                            + " lufs " + f1 (b["master"].value ("lufs", 0.0)) + "->" + f1 (a["master"].value ("lufs", 0.0));
            if (b.contains ("tracks"))
                for (auto& [k, tb] : b["tracks"].items())
                {
                    if (! a["tracks"].contains (k) || ! tb.is_object() || ! a["tracks"][k].is_object()) continue;
                    const auto& ta = a["tracks"][k];
                    const int t = std::stoi (k) - 1;
                    e += "; " + trackLabel (t, names) + " rms " + f1 (tb.value ("rms", 0.0)) + "->" + f1 (ta.value ("rms", 0.0));
                    if (tb.contains ("bands") && ta.contains ("bands"))
                        for (int g = 0; g < kAggBands && g < (int) tb["bands"].size() && g < (int) ta["bands"].size(); ++g)
                        {
                            const double d = ta["bands"][(size_t) g].get<double>() - tb["bands"][(size_t) g].get<double>();
                            if (std::fabs (d) >= 1.0) e += std::string (" ") + kAggBandNames[g] + (d > 0 ? "+" : "") + f1 (d, 0);
                        }
                }
            o << " effect: " << e << "\n";
        }
        else
            o << " effect: not measured yet\n";
    }
    return o.str();
}

std::string settingsSection (const PromptInputs& in)
{
    // Non-default params grouped by module: module(param=value ...); single-param modules as module=value.
    // v5 shows the master in full: its modules are the ones an agent sets to a value that happens to BE the default
    // (reverb decay 1.8 s, echo 250/375 ms), and hiding those made agents believe the call was dropped and re-send it.
    const bool fullMaster = in.promptVersion >= 5;
    auto line = [&] (int track0) {
        std::map<std::string, std::vector<const ParamSpec*>> mods;
        std::vector<std::string> order;
        for (auto& p : allParams())
        {
            if (p.track != track0) continue;
            if (track0 >= 0 && p.slot == Fader) continue; // shown as fdr in TRACKS
            const float v = in.paramValue (p.index);
            const float tol = p.domain == Domain::LogHz ? std::fabs (p.def) * 1e-3f : std::max (1e-4f, p.interval * 0.5f);
            if (std::fabs (v - p.def) <= tol && ! (fullMaster && track0 < 0)) continue; // below display resolution = default
            if (! mods.count (p.module)) order.push_back (p.module);
            mods[p.module].push_back (&p);
        }
        std::string s;
        for (auto& mod : order)
        {
            const auto& ps = mods[mod];
            std::string item;
            const bool single = ps.size() == 1
                                && (ps[0]->name == "gain" || ps[0]->name == "amount" || ps[0]->name == "value"
                                    || ((mod == "mute" || mod == "solo") && ps[0]->name == "on"));
            if (single)
            {
                const float v = in.paramValue (ps[0]->index);
                item = (mod == "mute" || mod == "solo") ? (v > 0.5f ? mod : "no " + mod) : mod + "=" + formatValue (*ps[0], v, false);
            }
            else
            {
                item = mod + "(";
                bool first = true;
                for (auto* p : ps)
                {
                    const float v = in.paramValue (p->index);
                    item += first ? "" : " ";
                    first = false;
                    item += p->name == "on" ? formatValue (*p, v, false) : p->name + "=" + formatValue (*p, v, false);
                }
                item += ")";
            }
            s += (s.empty() ? "" : " ") + item;
        }
        return s;
    };
    std::ostringstream o;
    o << "SETTINGS\n";
    const auto m = line (-1);
    o << "master: " << (m.empty() ? "defaults" : m) << "\n";
    std::string defaults;
    for (int t = 0; t < kMaxTracks; ++t)
    {
        if (! ((in.presentMask >> t) & 1u)) continue;
        const auto l = line (t);
        if (l.empty()) defaults += (defaults.empty() ? "" : ",") + std::to_string (t + 1);
        else o << trackLabel (t, in.names) << ": " << l << "\n";
    }
    if (! defaults.empty()) o << "all defaults: " << defaults << "\n";
    return o.str();
}

std::string triggerSection (const PromptInputs& in)
{
    std::string trig;
    if (in.trigger == "user") trig = "TRIGGER: user message: \"" + in.userMsg + "\"";
    else if (in.trigger == "stage") trig = "TRIGGER: stage edit: \"" + in.userMsg + "\"";
    else trig = "TRIGGER: periodic check (no user message)";
    if (in.promptVersion >= 4)
        return trig + "\nRespond with \"Intent: ...\" (first turn, or if it changes), 1-3 sentences on what you change and why, then tool calls.";
    return trig + "\nRespond with a 1-3 sentence rationale, then tool calls.";
}

BuiltPrompt buildPrompt (const PromptInputs& in)
{
    BuiltPrompt bp;
    bp.system = systemPrompt (in.promptVersion);
    bp.exactCount = static_cast<bool> (in.countTokens);
    bp.fixedTokens = in.fixedTokens >= 0 ? in.fixedTokens : fixedPrefixEstimate (in.promptVersion);

    const std::string head = (in.promptVersion >= 4 ? tracksSectionV4 (in) : in.promptVersion >= 3 ? tracksSectionV3 (in) : in.promptVersion >= 2 ? tracksSectionV2 (in) : tracksSection (in)) + lockedSection (in);
    const std::string tail = settingsSection (in) + triggerSection (in);

    std::vector<MemoryRecord> mem = in.memory;
    for (;;)
    {
        bp.user = head + memorySection (mem, in.names) + tail;
        bp.userTokens = count (in, bp.user);
        bp.estTokens = bp.fixedTokens + bp.userTokens;
        if (bp.estTokens <= in.tokenBudget || mem.empty()) break;
        mem.erase (mem.begin()); // drop oldest first
        ++bp.memoryDropped;
    }
    bp.memoryUsed = (int) mem.size();
    bp.messages = json::array ({ { { "role", "system" }, { "content", bp.system } }, { { "role", "user" }, { "content", bp.user } } });
    return bp;
}
} // namespace im
