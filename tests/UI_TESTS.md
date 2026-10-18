# Page tests (run by hand in a browser)

Run before a release, on a real multitrack (20+ files): `mix-final-actual serve <folder> --no-first-mix`, open the
printed link. Agent turns can be answered by any MCP client (`mix-final-actual mcp`: open_session, next_turn,
submit_turn, post_note). Pass 1: 2026-10-18 (first page), all 23 pass. Pass 2 (DAW page, cases 24-45): 2026-10-18,
21 tracks, Chrome on macOS; the cases marked * were not run in pass 2 (native dialogs, untouched code paths).

| # | Area | Do | Expect |
|---|------|----|--------|
| 1 | Load | open the link | one strip per file plus MASTER; group colour bars; clock shows the excerpt start |
| 2 | Transport | Play, Stop, space bar | meters move; time runs; space toggles (not while typing) |
| 3 | Loop | Loop off; click the timeline while stopped | playhead and clock jump to the click |
| 4 | Loop region | drag on the timeline; Play | loops inside the dragged region |
| 5 | Excerpt | double-click the timeline | region back to the excerpt, playhead at its start |
| 6 | Strip | drag a fader; double-click it; pan; double-click pan; M; S | engine values follow; solo silences the others |
| 7 | Detail | click a name; toggle a module; move a slider; pick a choice; double-click a slider | values reach the engine; double-click = default; panel fits without sideways scroll |
| 8 | Master | click MASTER | reverb, echo, bus comp, fader, limiter, five group buses |
| 9 | Rename | double-click a name, type, Enter | strip and panel renamed; group setting follows the track |
| 10 | Rename cancel | double-click, type, Escape | name unchanged |
| 11 | Rename refused | use another track's name (any case) | refused with a notice; blank names refused |
| 12 | Group | set a group in the panel | colour bar changes; "by name" goes back |
| 13 | Reopen | wait 10 s, restart without --new-mix | names, groups, settings, chat, loop all back |
| 14 | Export | -14 LUFS, stems, Export | progress, button disabled; mix.wav at -14.0 LUFS, 24-bit, song + 3 s; one stem per track, named as renamed |
| 15 | No agent | type a request, Enter | chat says it waits for the coding agent; status "waiting for your coding agent" (not "working") |
| 16 | Agent turn | answer it via MCP | chat shows the turn and its changes; faders move; status "agent connected" |
| 17 | A/B | A/B twice | before / after the last turn |
| 18 | Undo | Undo turn | values back; "(undone)"; A/B moves to the latest remaining turn or turns off |
| 19 | Mix it | press, answer 5 turns | five turns in the chat; a bad tool name is refused and can be resubmitted |
| 20 | First mix | start without --no-first-mix | waiting screen: turn n of N, waiting/answering text, progress bar |
| 21 | Mix by hand | press it during turn 2 | mixer opens with turn 1 kept; "Turn 2 stopped; nothing changed." |
| 22 | Mix by hand early | press it before any answer | mixer opens untouched, plays |
| 23 | Old link | open with a wrong token | "This link is out of date..." |
| 24 | Tracks view | open the page | waveform clips for every track, ruler with cycle and excerpt marks, headers with M/S, volume, pan, module tags |
| 25 | Trim | drag a clip's left edge | start and file offset move together; the engine has the new region |
| 26 | Fade | drag a clip's top corner handle | fade curve drawn; the engine has the fade |
| 27 | Move | drag a clip | moves in time; snaps to the playhead and other clip edges (Shift: no snap) |
| 28 | Split / delete | Cmd+E at the playhead, then Delete | two clips, then one; playback is silent where the clip was removed |
| 29 | Undo clips | Cmd+Z, Shift+Cmd+Z | the delete is undone and redone on the engine |
| 30 | Loudness | open a session at -14 | "measuring", then "master +N dB"; the master plays near -14 LUFS (peaks ~ -5 dBFS) |
| 31 | Reopen | restart | playback starts at the saved gain at once ("measuring" refines it) |
| 32 | Ask, no agent | type, Enter | turn card: listening (n / 8 s), then waiting with "No coding agent is listening" |
| 33 | Ask, agent | answer via MCP with two post_note calls | card shows "reading" with the notes, then the reply, notes and changes |
| 34 | Remix with goal | type, Cmd+Enter | "Remix (5 turns): goal" in the chat; the agent's turn text carries the goal |
| 35 | Stop | Stop on the live card (waiting, and while listening) | "Turn N stopped; nothing changed."; no further remix turns |
| 36 | Two sessions | MCP open_session(folder) then next_turn without folder | goes to that session, not "several sessions are open" |
| 37 | Export dialog | Cmd+B | default path in the Music folder; loudness follows playback; format switches the extension |
| 38 | Export FLAC + stems | export | FLAC 24-bit at the playback loudness (-14.0), stems in "<name> stems/" |
| 39* | Choose... / Show in folder | press them | the OS save dialog / file manager opens (macOS checked by the backend tests; Windows and Linux never run) |
| 40 | Mixer view | X | strips with EQ curve, block summaries, pan, fader, in/out/gr meters moving while playing |
| 41 | Strip block | click a row; Alt-click a row with an on/off | inspector opens at that block; the block turns on/off |
| 42 | EQ editor | drag a band in the inspector curve; Cmd+Z | frequency and gain follow; the strip summary updates; undo restores both |
| 43 | Keys | ?, Esc, Up/Down, M, C, Z, =, X, Space | as listed in the page's shortcut window |
| 44* | First mix | start without --no-first-mix | the waiting screen as in cases 20-22 (unchanged code) |
| 45* | Narrow window | under 1000 px wide | not designed for it yet; only the inspector can be hidden (I) |
