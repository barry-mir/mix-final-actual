# Bouncer

A mixing console in your browser, mixed by your coding agent. Give it a folder of tracks with proper names
("Lead Vocal.wav", "Kick.wav", ...); your agent (Claude Code or Codex) makes the mix by reading the console's
measurements turn by turn, then you keep adjusting it and hear every change at once.

Early work in progress: the playable mixer page works; the agent bridge is next (see [docs/DESIGN.md](docs/DESIGN.md)).

## Build and run

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bouncer_artefacts/Release/bouncer path/to/tracks      # opens the page in your browser
```
Options: `--port N` (default: any free port), `--no-open` (print the URL only).

Linux needs: `build-essential cmake ninja-build libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev
libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libxcomposite-dev libgl1-mesa-dev`.

## Licence

AGPLv3 (JUCE 8 is used under its AGPLv3 licence).
