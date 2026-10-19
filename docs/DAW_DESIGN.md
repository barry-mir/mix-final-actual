# The mixer page as a DAW: design

Written 2026-10-18 from user feedback on the first page (items numbered as in that feedback). This file is the
contract between the engine/server (C++) and the page (web/); both sides are built from it.

## 1. What you hear is what you export

Problem: the live page plays the mix as mixed (the Pocket Knife example sat at -37 LUFS) while Export masters it to
-14 LUFS, so the two sound nothing alike.

Design: one **Loudness** setting (transport bar: `as mixed`, `-16`, `-14 (streaming)`, `-9 (loud)`), used by playback
*and* export. When a target is set, the engine applies a delivery gain before the master limiter, exactly as export
does, so live playback and the exported file are the same audio.

- The gain is computed by one function used by both: render the whole song offline with the current settings
  (delivery 0), measure integrated loudness, set gain = target - measured, re-render to correct for the limiter
  (at most 3 passes, stop within 0.2 LU). About 11 s per pass for 21 tracks x 4 min.
- Live: a background measurer re-runs it about 1.5 s after the settings or edits stop changing (a change during a
  run restarts it after that run). The live gain glides to the new value over 1 s. While a measurement is pending the
  page shows "re-measuring"; the last gain stays in effect.
- Export reuses the live gain when it was measured for exactly the current settings; otherwise it measures first.
  Either way the export is rendered with the gain the page plays, so they match.
- The agent must keep measuring the mix *as mixed* (the paper's method). The engine therefore runs a second limiter
  on the un-gained signal for the analysis tap whenever the delivery gain is not 0 dB. The agent never sees the
  delivery gain.

## 2. Seeing the agent work

Problem: after typing a request nothing visible happens until the answer lands, and when no agent is listening the
page gives little sign of it.

Design: each turn is a **turn card** in the Assistant panel, updated live through these stages:

| stage | shown as |
|---|---|
| `listening` | "Listening to the loop 6 / 8 s": the console plays the loop region so the measurements reflect the current mix |
| `waiting` | "Sent to your coding agent; waiting for it to pick the turn up" (+ how to connect when no agent has polled for 60 s) |
| `reading` | "Agent is working 14 s", with the agent's **notes** streaming in as it posts them |
| `applying` | "Applying 17 changes" |
| `done` | the reply, the change list (with Undo turn), and "what the agent read" (the MIX lines of the measurements) |

- Notes: a new MCP tool `post_note(turn, text)`. The skill tells the agent to post short notes while it works (what
  it reads in the measurements, then its plan) before `submit_turn`. Notes are chat entries with role `note` and the
  turn's tick.
- Live listening fix: a live turn waits until the console has played `listen` seconds (default 8) of audio after the
  last change from anyone (agent, page controls, clip edits), starting playback if stopped. Before this, back-to-back
  live turns measured audio from before the previous turn's changes. The offline first mix is unchanged.

## 3. Export: choose where

- Export opens a dialog: file path (default `<Music>/mix_final_ACTUAL/<tracks folder name>/<tracks folder name>.wav`,
  `<Music>` = the OS music folder; the last folder used is remembered), format (WAV 24-bit or FLAC 24-bit), loudness
  (the same setting as playback), and stems (written to `<file name> stems/` next to the file).
- "Choose..." opens the OS's own save dialog through the server: macOS `osascript` (choose file name), Windows
  PowerShell SaveFileDialog, Linux `zenity` or `kdialog`. If none is available the path field is typed by hand.
- After export: "Show in Finder / Explorer / file manager" and the measured loudness.

## 4. Ask and Remix

The two buttons were "Send" (one turn on the typed request) and "Mix it" (a full 5-turn pass). Renamed and explained:

- **Ask** (Enter): one turn; the agent changes what you asked for.
- **Remix x5**: five turns from the current mix, the same method as the first mix. If the box has text, it is the goal
  for all five turns.

## 5. More per-track information (as in the plugin)

Every mixer strip, like the plugin's strip: number chip and group colour, name, an **EQ/filter response curve**, then
one row per processing block with its live summary (`HP 80 Hz`, `EQ LF +2 . HMF -3`, `Comp 3:1 . -20 dB`, `Gate off`,
`De-ess off`, `Drive off`, `Doubler off`, `Pan L30 . W 1.0`, `Rev -12 . Echo off`, `-> Vocals bus`). Off blocks are
dimmed. Clicking a row opens that block in the Inspector; Alt-click toggles the block on or off. Meters: input,
output (peak + RMS) and gain reduction (compressor and gate).

## 6. The DAW layout

```
+---------------------------------------------------------------------------------------------------+
| > [] (C) 1:23.4 / 3:56.2   Loudness [-14 v] +24.1 dB   A/B   [Tracks|Mixer]   Export...  agent: o  ? |
+-------------+------------------------------------------------------------------+------------------+
| INSPECTOR   | ruler  0:00   0:30 [=== cycle ===] 1:30  ...  (^ agent listens here)  | ASSISTANT        |
| track name  +-------------+----------------------------------------------------+ turn cards       |
| group       | 1 Kick  M S | [~~~~~~~~ waveform clip ~~~~~~~~]      [~~~~ clip ~~~]| ...              |
| clip: gain, | vol ---o--  |                                                    |                  |
| fades, pos  | 2 Snare M S | [~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~]   | [ask box       ] |
| modules...  | ...         |                                                    | Ask   Remix x5   |
+-------------+-------------+----------------------------------------------------+------------------+
```

- Views: **Tracks** (arrangement) and **Mixer** (strips from item 5), switched with X. Inspector (I) on the left,
  Assistant on the right; both collapsible.
- Ruler: click to place the playhead; drag to set the cycle (loop) region; a marker shows the excerpt the agent
  listens to (Shift+C returns the cycle to it).
- Track header: number, group colour, name (double-click renames), M, S, volume and pan mini-sliders, meter,
  gain-reduction dot, module dots (which blocks are on).
- Lanes: one or more **clips** per track, drawn with the track's waveform in its group colour.
  - drag the body: move in time; drag the left/right edge: trim; drag the top corners: fade in/out;
    drag the gain line (Alt+drag body vertically): clip gain; Cmd/Ctrl+E: split at the playhead; Delete: remove clip.
  - Shift while dragging: fine (no snap). Otherwise moves and trims snap to 10 ms and to other clips' edges and the
    playhead within 8 px.
- Zoom: Cmd/Ctrl+wheel or pinch, = and - keys, Z fits the song. Lanes scroll vertically with their headers; the view
  follows the playhead while playing.
- Every control: drag, Shift/Cmd-drag fine, wheel to nudge, double-click or Alt-click resets to default, a value
  tooltip while dragging. Undo/redo covers control changes and clip edits by the user (agent turns keep their own
  Undo turn button).

### Clip model (engine)

Per track, a list of regions in timeline seconds; the default is one region covering the whole file at 0 (exactly the
old behaviour):

```json
{ "pos": 12.5, "src": 3.0, "len": 40.0, "gain": -2.0, "fadeIn": 0.05, "fadeOut": 0.5 }
```

`pos` = start on the timeline, `src` = start inside the file, `len` = length; `gain` dB; fades in seconds (equal-power
curves). Regions on one track never overlap (the server sorts and clips overlaps, later region wins). The edit list
is part of the processor state, so it is saved in the project, used by export, by the loudness measurer and by the
agent's listening. The song length is the end of the last region (at least the longest file).

## 7. Key bindings

Chosen from Logic Pro, Pro Tools and Ableton conventions; Cmd on macOS = Ctrl on Windows/Linux. None fire while
typing in a text field. `?` shows this table in the page.

| key | action |
|---|---|
| Space | play / stop |
| Return | go to the start of the cycle (or of the song when the cycle is off) |
| Home / End | go to the song start / end |
| , and . (or Left / Right) | playhead back / forward 1 s; with Shift 5 s |
| C | cycle (loop) on / off |
| Shift+C | cycle back to the agent's excerpt |
| Up / Down | select the previous / next track |
| M / S | mute / solo the selected track |
| Cmd+E | split the selected clip (or the selected track's clip under the playhead) at the playhead |
| Delete / Backspace | delete the selected clip |
| Cmd+Z / Shift+Cmd+Z (Ctrl+Y) | undo / redo |
| = / - | zoom in / out |
| Z | zoom to fit the song |
| X | Tracks / Mixer view |
| I | inspector on / off |
| A | A/B: before / after the agent's last turn |
| / | type a request to the agent |
| Cmd+Enter | in the request box: Remix x5 with this goal |
| Cmd+B | Export... (Logic's bounce) |
| Cmd+S | save the project now |
| Esc | leave the text field, close a dialog, clear the selection |
| Alt+click a control | reset to default |

## 8. README

The Codex install gets a code block like Claude Code's.

## Server API (additions and changes)

All under `/api/`, token as before. Times in seconds.

- `GET session`: as before, plus `tracks[i].len` (file length) and `peaks_block` (frames per peak value),
  `rate` (session sample rate).
- `GET peaks?track=i`: `application/octet-stream`, one byte per `peaks_block` frames: the max |sample| of both
  channels, encoded as `round(255 * sqrt(peak))` (clamped).
- `GET edits` -> `{ "version": n, "tracks": [[region...]...] }`; `POST edits {"track": t, "regions": [...]}`
  replaces one track's list (validated: lengths > 0.01 s, inside the file, fades <= len/2, gain -48..+24) and returns
  the stored list.
- `GET status`: as before, plus `song_len` (timeline length), `meters`: `[{in, out, out_rms, gr}]` per track (linear
  peaks, gr in dB >= 0, the larger of compressor and gate), `loudness`:
  `{target: -14 | null, gain_db, state: "off" | "measuring" | "ready", measured_lufs}`, `turn`:
  `{tick, stage, listen_s, listen_need_s, since_s, changes}` or null.
- `POST loudness {"target": -14 | null}`.
- `POST prompt {"text"}` (Ask); `POST mix {"turns": 5, "goal": "..."}` (Remix).
- `POST export {"path", "format": "wav" | "flac", "stems", "loudness"}` (loudness falls back to the playback setting);
  `GET export` adds `file`. `POST choose_save {"path"}` -> `{"path"}`, `{"cancelled": true}` or
  `{"unavailable": true}` (blocks while the OS dialog is open). `POST reveal {"path"}`.
  `GET export_defaults` -> `{"path"}`.
- MCP: new tool `post_note(turn, text)`; HTTP `POST agent/note {"turn", "text"}`. Notes appear in `GET chat` as
  entries `{role: "note", tick, text}`.

## As built (2026-10-18)

Differences from the plan above, found while building and testing:

- **Stop**: the live turn card has a Stop button (`POST stop_turns`): the turn waiting for the agent, or still
  listening, ends with nothing changed, and the rest of a remix is dropped. Without it a request with no agent around
  waited 30 minutes before it was skipped.
- The live card is patched in place while its timers run and rebuilt only when the stage changes (rebuilding every
  second swapped the Stop button out from under the pointer).
- MCP: calls that name no folder go to the session this MCP process opened last (before, with two sessions open, the
  agent's `next_turn` was refused with "several sessions are open").
- Loudness: the project also saves the playback gain (`loudness_gain_db`), so a reopened session plays at the right
  level at once instead of quiet until the first measurement (5-25 s). Soloing does not trigger a re-measure; with a
  solo on, export measures for itself. Mastering takes up to 4 passes with a slope-aware step (3 plain steps stalled
  0.4 LU short at -9 LUFS).
- The timeline shows 14 px before 0:00 so a clip that starts at the top can be grabbed by its left edge.
- Mixer strips are 138 px wide so the block summaries fit.

## Round 2 (backend, 2026-10-18)

From the second round of user feedback. What the server does now, and its API as built.

### Offline listening (no sound)

Live agent turns no longer start playback or wait for played audio. Before each turn the console renders the
**analysis range** offline in a second engine (built once, tracks memory-mapped, reused; the live settings, clip edits,
names and groups are copied into it each turn; solos are cleared, so the agent measures the mix), faster than real time,
and the agent's measurements come from that render, including the group-bus readings and the previous turn's
"features after". The range is the cycle (loop) region when it is at least 2 s, else the excerpt, or the whole song
when the page asks for it. The render plays the last 3 s of the range first (fills the 3 s analysis window), then the
whole range once, so the long-term averages cover the range and the short-window features its end. A whole-song render
closes with 3 s of the excerpt instead (songs end in silence, which made the agent read "silent"). The first mix is
unchanged. Cost on 21 tracks x 4 min: about 6 s for a 30 s range (the first one includes building the engine), about
35 s for the whole song (mostly the analyzer's per-second features, not the audio).

- `POST analysis_range {"whole": bool}` -> `{analysis_whole, analysis_range: [a, b]}`; saved in the project.
- `GET status`: `analysis_range: [a, b]` (what the next turn analyzes), `analysis_whole`; while listening,
  `turn: {stage: "listening", listen_s (rendered seconds of the range), listen_need_s (range length), range: [a, b]}`
  (`range` is the last turn's range at other stages).

### The console starts the coding agent

When a turn has waited 5 s unanswered and no agent has polled in 60 s (or the last one seen was an agent the console
started, which has exited), the console starts one headless, one at a time:

- Claude Code: `claude -p "<prompt>" --mcp-config <appdata>/agent-mcp.json --strict-mcp-config --allowedTools
  mcp__mix-final-actual__{open_session,next_turn,submit_turn,post_note,session_status}`.
- Codex: `codex exec --ephemeral --skip-git-repo-check -s read-only -c mcp_servers.mix-final-actual.command=... -c
  ...args=["mcp"] -c ...tool_timeout_sec=120 -c ...default_tools_approval_mode="approve" "<prompt>"`. (Not
  `--approve-for-me`: it writes a trust entry for the work folder into the user's `~/.codex/config.toml`.)
- Which: the kind of the agent that talks to this console (the MCP front sends `POST agent/hello {client, path}` with
  its `clientInfo.name` and PATH), else the last agent that used the mix tools (`<appdata>/agent.json`, written at MCP
  initialize), else `claude`, else `codex`; looked up on that agent's PATH, this PATH, `~/.local/bin`,
  `/opt/homebrew/bin`, `/usr/local/bin`, `~/.npm-global/bin`, `~/.claude/local`, `~/.bun/bin`, `%APPDATA%\npm`.
  The child runs with that PATH plus the system folders (Claude Code needs `/usr/bin/security` for its login).
- The prompt: answer the waiting turns for the folder as the skill's section 2 says (1-3 notes, one submit per turn),
  until `next_turn` says no turn twice. Runs in `<appdata>/agent-work`, output to `<appdata>/logs/agent-<time>.log`
  (first line: the exact command). `MFA_AGENT_MODEL` picks a model (tests); `MFA_AUTO_AGENT=0` turns the feature off.
- `POST stop_turns` also kills it (its process group, so its MCP child goes too).
- If it exits without answering, the turn ends with its last words, e.g. "Turn 10 skipped: the coding agent (claude)
  exited without answering this turn (it said: Not logged in · Please run /login); log: ...".
- If no CLI is found: after 30 s the turn ends with "no coding agent is connected, and Claude Code or Codex could not be
  found to start one. In Claude Code or Codex, ask it to answer the mixer ...". The "Waiting for your coding agent" chat
  line is only added when no agent can be started.
- `GET status`: `auto_agent: {state: none|starting|running|exited|unavailable, cli, exe, since_s, error, log}`.
- The default turn timeout is 600 s (was 1800).

Tested: Claude Code (sonnet) and Codex each answered a page request end to end (notes, then changes) with no agent
connected; Stop killed a running one; a not-logged-in run and the no-CLI case end the turn with the reasons above;
`~/.codex/config.toml` unchanged.

### Tempo

At load (or when a project without a saved tempo opens), in the background: spectral-flux onsets of the drum tracks
(else all tracks) over 60 s around the excerpt; the strongest periodicity over 60-200 BPM, then a comb search (+-4%,
0.02 BPM) for the exact period and beat phase; its octaves compete on the comb score and one inside 90-160 BPM wins only
when it is nearly as strong (a 4/3 shuffle period never competes); bar 1 = the beat with the most energy just after it.
Pocket Knife reads 83.96 BPM (a precise comb on the snare alone: 84.0; librosa's quantized estimate: 83.4).

- `GET tempo` and `session.tempo`: `{bpm, beats_per_bar, offset_s (bar 1, seconds from the song's start), detected,
  confidence, state: detecting|ready|none}`.
- `POST tempo {bpm? (20..400), beats_per_bar? (1..16), offset_s? (>= 0)}` (detected becomes false) or `{"redetect": true}`.
  Saved in the project.

### Clips: overlaps, crossfades, pasting across tracks

Overlapping clips on a track are kept and sum, each with its own (equal-power) fades: the page makes a crossfade by
overlapping two clips with fades over the overlap. A clip has `srcTrack` (whose file it plays; default its own track),
so a clip can be pasted on any track and goes through that track's strip. Validation: `srcTrack` must be a track of the
session and `src + len` must fit that track's file; up to 512 clips per track. `GET/POST edits` regions carry
`srcTrack` (always, resolved).

### Spectrum (EQ view)

- `POST scope {"track": t | -1 (master) | null}`: the engine copies only that track's input and output (master: the mix
  before the glue compressor and limiter, and the output) into two lock-free rings.
- `GET spectrum` -> `{track, hz: [120 band centres, 20 Hz .. 19.4 kHz, 1/12 octave], pre: [dB], post: [dB]}`: 4096-point
  Hann FFT, each band's strongest bin, 0 dB = a full-scale sine; rises at once, falls with a 300 ms time constant
  between requests; floor -120 (also when stopped).

### Meters

`GET status` adds `buses: [{peak, rms, gr}]` for the five group buses (vocals, drums, bass, guitars, keys; linear
output, this block), `fx: {reverb: {peak}, echo: {peak}}`, `master_lufs_m` (momentary loudness of what plays, 400 ms).

### Project name, reopening later

- `session.name` (default: the folder name); `POST project {"name"}` (trimmed, non-empty, up to 120 chars). Saved in the
  project; the export default file uses it; the recent list shows it.
- `<appdata>/recent.json` (newest first, 20): `GET recent` -> `[{folder, name, last_opened, url (if running), current,
  exists}]`; `POST open {"folder"}` -> `{url, folder}` (its running console, else a new one started with `--no-open`;
  waits up to 30 s).
- `mix-final-actual open [folder]` (no folder = the latest mix): attaches or starts, prints and opens the page.
- MCP: `list_projects`; `open_session` reopens a saved project as it was.
- The console keeps running when the page closes or the agent exits. Playback stops when no page has polled
  `GET status` for 20 s.

### A/B

- `POST compare {"raw": bool}`: plays the unprocessed sum of the tracks, level-matched to the mix (up to +-20 dB now,
  for playback at a delivery loudness), 20 ms crossfade. The analysis tap and the loudness gain are unaffected.
  `status.compare_raw`.
- Loudness: the settings key now hashes the clip edits' content (not their version), and the last 8 measurements are
  cached by settings: returning to a measured state (undo, A/B of a turn) applies its gain at once.
- `POST loudness {"hold": bool}` (target optional now): while held, no re-measuring and no cached jumps;
  `loudness.state = "held"`, `loudness.held`.

### Removed

The live "wait for fresh played audio" path and its auto-play (`waitForFreshAudio`), and the `setPlaying(true)` in
`POST prompt` / `POST mix`.
