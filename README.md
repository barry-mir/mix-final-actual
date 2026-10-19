# mix_final_ACTUAL.wav

<p align="center"><img src="docs/The%20final%20mix.png" width="480" alt="Comic: a tired engineer at 4:47 AM, a folder of crossed-out mix_final files ending in mix_final_ACTUAL.wav. Narrator: it was not the final mix."></p>

A mixing console in your browser, mixed by your coding agent. Give it a folder of tracks with proper names
("Lead Vocal.wav", "Kick.wav", ...); your agent (Claude Code or Codex) makes the mix by reading the console's
measurements turn by turn, then you keep adjusting it and hear every change at once.

See a real run: [docs/examples/pocket-knife.md](docs/examples/pocket-knife.md) (a 21-track band mixed by Claude Code
in five turns, 2 min 18 s). How it is built: [docs/DESIGN.md](docs/DESIGN.md).

## Install

**Claude Code**
```
/plugin install mix-final-actual --marketplace barry-mir/mix-final-actual
```
or from a shell: `claude plugin marketplace add barry-mir/mix-final-actual` then
`claude plugin install mix-final-actual@mix-final-actual`.

**Codex** (desktop app or CLI)
```
codex plugin marketplace add barry-mir/mix-final-actual
```
then install `mix-final-actual` from the plugin list (`/plugins` in the CLI).

Then ask your agent: "mix the tracks in ~/Music/my-song". It makes the first mix (5 turns), the mixer page opens in your
browser, and requests you type there go to your agent. The page plays at the loudness you export at (-14 LUFS for
streaming by default), so what you hear is what Export writes. The page works like a DAW: a Tracks view with waveforms
(move, trim, fade, split and clip gain), a Mixer view, an inspector with an EQ editor, and the usual shortcuts (press `?`
in the page for the list). Export asks where to save (default
`Music/mix_final_ACTUAL/<song>/`), as 24-bit WAV or FLAC, optionally with stems.

The plugin ships native binaries for macOS (universal), Linux (x64, arm64) and Windows (x64), built by CI on the
`dist` branch. Nothing to install besides the plugin; no API keys; everything runs locally.

## What it writes, and where

- Next to your tracks: `mix_final_ACTUAL project.json` (the mix and your clip edits, saved every 10 s; reopened next
  time). Your audio files are never changed.
- Exports: where you choose (default `Music/mix_final_ACTUAL/<song>/<song>.wav`, stems in `<song> stems/` next to it).
- Per user (`~/Library/Application Support/mix-final-actual` on macOS, `~/.config/mix-final-actual` on Linux,
  `%APPDATA%\mix-final-actual` on Windows): `instances/` (how the agent finds a running mixer; removed on exit),
  `turns/` (each turn's text), `logs/`, `settings.json` (the last export folder), and `cache/` (converted copies of non-WAV tracks or tracks at another sample
  rate; safe to delete).
- Nothing leaves your machine. The page and the agent tools only answer on 127.0.0.1, with a random token per session.

## Coming back to a mix later

The console keeps running when you close the page or your agent exits; open the page again from the link your agent
gave you. After a restart, reopen a mix in either of these ways (a saved project opens exactly as you left it):

- ask your agent: "open my Pocket Knife mix again" (it lists earlier mixes with `list_projects`), or
- run `mix-final-actual open` (the latest mix) or `mix-final-actual open ~/Music/my-song`; in the plugin folder the program
  is `bin/mix-final-actual`.

Requests you type in the page need a coding agent to answer them. If none is connected (it exited, or it is busy with
something else), the console starts your agent itself in the background: `claude -p` or `codex exec`, whichever you
used, with only the mix tools, on your own login. Set `MFA_AUTO_AGENT=0` in the environment to turn that off.

## Troubleshooting

- **The page says "waiting for your coding agent"**: ask your agent to continue the mix (it answers turns with the
  `next_turn` / `submit_turn` tools). Typed prompts also wait for the agent.
- **Track names**: the agent mixes by name, so name files by instrument (`Kick In.wav`, `Lead Vocal.wav`). Rename
  a track in the page by double-clicking its name; set its instrument group in the track's panel.
- **Start over**: delete `mix_final_ACTUAL project.json`, or ask the agent for a new mix (`open_session` with
  `new_mix`).
- **Logs**: `logs/serve.log` in the per-user folder above.

## Build and run

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/mix-final-actual_artefacts/Release/mix-final-actual serve path/to/tracks
```
`serve` first makes the mix with your coding agent (5 turns, answered through the MCP tools below), then opens the
mixer page. Options: `--turns N`, `--no-first-mix` (mix by hand only), `--port N`, `--no-open`.

Connect a coding agent: `mix-final-actual mcp` is an MCP server on stdio. For example in Claude Code:
`claude mcp add mix-final-actual -- /path/to/mix-final-actual mcp`, then ask it to mix a folder.

Linux needs: `build-essential cmake ninja-build libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev
libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libxcomposite-dev libgl1-mesa-dev`.

## Licence

AGPLv3 (JUCE 8 is used under its AGPLv3 licence).
