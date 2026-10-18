# Page tests (run by hand in a browser)

Run before a release, on a real multitrack (20+ files): `mix-final-actual serve <folder> --no-first-mix`, open the
printed link. Agent turns can be answered by any MCP client (`mix-final-actual mcp`: open_session, next_turn,
submit_turn). Last full pass: 2026-10-18, 21 tracks, Chrome on macOS — all pass.

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
