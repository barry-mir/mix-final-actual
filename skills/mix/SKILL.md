---
name: mix
description: Mix a multitrack (a folder of audio files, one per instrument, such as stems or raw recordings) into a finished stereo mix on a real mixing console, then open a mixer page in the browser where the user tweaks it live and asks for changes. Use when the user wants tracks or stems mixed, a rough mix, a balance, or to keep working on a mix made this way.
---

# Mix a multitrack (mix_final_ACTUAL.wav)

You mix on a real DSP console through the `mix-final-actual` MCP tools. You never hear audio. Every turn the console
plays the music, measures it, and sends you a text report (levels, dynamics, stereo width, tone in 8 bands, which track
owns each band, instrument-group totals, and how all of it compares with finished professional mixes) together with the
console's tools. You answer with tool calls; the console applies them, plays again, and the next turn shows you what
your moves did.

## 1. Open the session

- Get the folder of tracks from the user (absolute path). Each file is one track; the file name is the track's name,
  so names like `Lead Vocal.wav`, `Kick In.wav`, `Bass DI.wav` matter. If names are cryptic (`Audio 01.wav`), ask the
  user what each track is and suggest they rename the files first.
- Call `open_session(folder)`. The console picks a 30 s excerpt where every instrument plays and starts the first mix
  (5 turns). If the folder already has a saved mix (`mix_final_ACTUAL project.json`, saved automatically), it reopens
  live as the user left it; tell the user, and only start over with `open_session(folder, new_mix=true)` if they ask
  (close the open session first).

## 2. Answer turns

Loop: `next_turn` → answer → `submit_turn`.

- `next_turn` returns one turn: `=== SYSTEM ===` (how to mix and how to answer: follow it exactly), `=== USER ===`
  (this turn's measurements, your last turns and what they changed, and the user's request if any) and
  `=== TOOLS ===` (the console functions). It can be long; read all of it. It is also saved to the file path printed
  at its end.
- Before answering, think it through: what kind of song this is and what it needs, how the tracks relate (levels, band
  shares, conflicts), what your earlier turns did and what they changed, and which moves follow from that.
- While you work, post 2-4 short notes with `post_note(turn, text)`: first what you read in the measurements (one or two
  sentences, e.g. "Guitars sit 4 dB above their range and own the low-mids; vocals are 3 dB under."), then your plan.
  The user watches these live in the mixer page, as if watching you think. Keep them plain and short; then submit.
- Answer with ONE `submit_turn(turn, content, tool_calls)`: `content` is the reply the SYSTEM part asks for, and
  `tool_calls` is every console call of the turn, `[{"name": "set_fader", "arguments": {"track": "Lead Vocal", "db": -3}}, ...]`,
  following the TOOLS schemas. Turn 1 usually needs many calls (a whole mix); later turns fix what the measurements
  flag. If nothing should change, call the `no_change` tool.
- If `submit_turn` reports an error (unknown tool, bad arguments), fix the answer and submit the same turn again.
- If your context is tight or you can hand work to a helper agent, give each turn to a fresh helper with these
  instructions and the turn's file path; each turn carries its own memory of the earlier turns.

After the 5th turn of the first mix, the mixer page opens in the user's browser. Tell the user, briefly, what the mix
does (a sentence or two from your turns).

## 3. Stay with the user

The user can now play the mix, change any control, edit clips, loop another part of the song, and type requests in
the page: **Ask** ("vocals more upfront", "less reverb on the drums") runs one turn on that request; **Remix** runs another
full 5-turn pass, toward the text in the box if there is any (the turns show it as the user's goal). Each of those
arrives as a turn through `next_turn`. Before a live turn the console plays a few seconds of the loop, so the
measurements describe the mix as it is now. Keep calling `next_turn` (it waits up to 45 s each call, then returns
"No turn waiting") and answer every turn as above, until the user tells you they are done. Requests may also come to
you directly in this conversation: pass them on by answering the next turn with that in mind, or tell the user to type
it in the page.

`session_status` tells you where the session is; `close_session` stops the console (the page stops working), only when
the user asks.

## Notes

- Everything runs on the user's machine; nothing is uploaded.
- The console's mix lives in the running app. The page has the controls to export audio.
