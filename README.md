# mix_final_ACTUAL.wav

A mixing console in your browser, mixed by your coding agent. Give it a folder of tracks with proper names
("Lead Vocal.wav", "Kick.wav", ...); your agent (Claude Code or Codex) makes the mix by reading the console's
measurements turn by turn, then you keep adjusting it and hear every change at once.

Work in progress (see [docs/DESIGN.md](docs/DESIGN.md)): the mixer page and the agent bridge work; the session maker,
the plugin packaging and the release builds are next.

## Install

**Claude Code**
```
/plugin install mix-final-actual --marketplace barry-mir/mix-final-actual
```
or from a shell: `claude plugin marketplace add barry-mir/mix-final-actual` then
`claude plugin install mix-final-actual@mix-final-actual`.

**Codex** (desktop app or CLI): `codex plugin marketplace add barry-mir/mix-final-actual`, then install
`mix-final-actual` from the plugin list.

Then ask your agent: "mix the tracks in ~/Music/my-song". It makes the first mix (5 turns), the mixer page opens in your
browser, and prompts you type there go to your agent. Export renders the whole song (and stems) next to your tracks, at a delivery loudness of your choice
(-14 LUFS for streaming by default, limiter at the console's ceiling).

The plugin ships native binaries for macOS (universal), Linux (x64, arm64) and Windows (x64), built by CI on the
`dist` branch. Nothing to install besides the plugin; no API keys; everything runs locally.

## What it writes, and where

- Next to your tracks: `mix_final_ACTUAL project.json` (the mix, saved every 10 s; reopened next time) and
  `mix_final_ACTUAL exports/<date time>/` (`mix.wav`, and `stems/` if asked). Your audio files are never changed.
- Per user (`~/Library/Application Support/mix-final-actual` on macOS, `~/.config/mix-final-actual` on Linux,
  `%APPDATA%\mix-final-actual` on Windows): `instances/` (how the agent finds a running mixer; removed on exit),
  `turns/` (each turn's text), `logs/`, and `cache/` (converted copies of non-WAV tracks or tracks at another sample
  rate; safe to delete).
- Nothing leaves your machine. The page and the agent tools only answer on 127.0.0.1, with a random token per session.

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
