# mix_final_ACTUAL.wav

A mixing console in your browser, mixed by your coding agent. Give it a folder of tracks with proper names
("Lead Vocal.wav", "Kick.wav", ...); your agent (Claude Code or Codex) makes the mix by reading the console's
measurements turn by turn, then you keep adjusting it and hear every change at once.

Work in progress (see [docs/DESIGN.md](docs/DESIGN.md)): the mixer page and the agent bridge work; the session maker,
the plugin packaging and the release builds are next.

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
