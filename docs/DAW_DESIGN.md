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
