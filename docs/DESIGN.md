# mix_final_ACTUAL.wav: design

mix_final_ACTUAL.wav mixes a folder of named tracks. The user's coding agent (Claude Code or Codex) makes the mix through a
measured, multi-turn dialog with a real DSP console, and the user then keeps working on it in a mixer page in the
browser, hearing every change at once. Free and open source.

## Decisions (2026-10-18)

| Topic | Decision |
|---|---|
| Who answers prompts | The coding agent the user already runs (Claude Code / Codex), through the plugin's MCP tools. No API keys anywhere, no built-in model. |
| Output | Audio (full-song WAV/FLAC, stems) and a project file (settings + change history) that reopens in the app. No REAPER project. |
| UI | A web page served by the engine on 127.0.0.1, with a random session token on every API call and a loopback Host check. |
| Python | None in the user's path: everything ships in the native binary. |
| First mix | Made before the page opens: excerpt chosen by the same rule as now, then the 5-turn dialog. The user can pick another window later in the page. |
| Turns | Each prompt from the user runs one turn; a "mix it" action runs the 5-turn method. |
| Console | The full console, group buses included (not the paper mode). |
| Track count | No fixed limit. |
| Input | A folder, or files dropped on the page; names from the file names, renamable; groups guessed from names, correctable. |
| No agent connected | The page still works as a manual mixer and says how to connect one. |
| Platforms | macOS (arm64, x64), Windows, Linux. No Apple developer account: the plugin downloads the binary with curl, so it carries no quarantine flag. |
| Name | Display name `mix_final_ACTUAL.wav`; id `mix-final-actual` (skill, plugin, binary, repo: ids allow only lowercase, digits and hyphens). |
| Licence | JUCE 8 is used under its free licence, AGPLv3, so the project is AGPLv3. |
| Data credits | The mix statistics are described only as statistics over professional mixes. |

## Architecture

```
mix-final-actual  (one native binary)
  ├─ engine      MixEngine + MultitrackPlayer, real time to the sound card
  ├─ analysis    Analyzer: the features the agent reads every turn
  ├─ agent loop  Agent / PromptBuilder / Tools: builds each turn's request, validates and applies the tool calls
  ├─ web         HTTP API + the page (compiled in)                       app/WebServer.cpp, web/index.html
  └─ bridge      Bridge + `mix-final-actual mcp` (stdio MCP front)      src/agent/Bridge.cpp, app/Mcp.cpp
```

The agent loop already builds a complete request (system prompt, the turn's measurements and memory, tool
schemas) and parses a reply with tool calls. Today that goes to an OpenAI-style HTTP endpoint (`LlmClient`). The bridge
replaces it: `Agent::runTick` hands the request to the bridge and waits; the coding agent fetches it with an MCP tool,
answers with all of the turn's tool calls in one submit, and the turn continues as before. One submit per turn
matters: turn 1 makes about 95 tool calls, and 95 separate MCP calls would mean 95 model steps.

Process lifetime: an MCP server over stdio dies with the agent session, but the page should stay open. So
`mix-final-actual mcp` is a thin stdio front that starts (or finds) a long-lived `mix-final-actual` process and forwards to it.

## Milestones

1. **Playable page** (done): load a folder, play, loop, seek; strips with fader, pan, mute, solo, meters; every
   parameter of a track or the master in a detail panel generated from the parameter registry.
2. **Agent bridge** (done): `LlmClient` / `ServerManager` replaced by the bridge; MCP tools `open_session`, `next_turn`,
   `submit_turn`, `session_status`, `close_session`; the first mix runs offline before the page opens; prompts and
   "Mix it" from the page become turns; per-turn undo in the page.
3. **Session maker in C++** (done, in the player's load): track names from file names (unique), the 30 s excerpt
   where the quietest instrument group is loudest, one gain putting the excerpt's unity sum at -23 LUFS, the excerpt
   as the default loop region. Checked against the Python maker on a 21-track song: excerpt 1 s later (frame
   alignment), gain within 0.1 dB. Silent tracks are kept (they may play outside the excerpt).
4. **No track limit** (done): the track count is set per process from the session (`setMaxTracks`) before any engine
   exists; every per-track array is sized from it; the present-track mask is a count (tracks 0..n-1). Tracks are not
   loaded into RAM: WAV/AIFF are memory-mapped in place (prefetch thread ahead of the playhead), other formats/rates are
   converted once into a cached float WAV and mapped. Checked: 105 tracks x 236 s load in 3 s with a 462 MB footprint
   (it was ~9.5 GB), real time at 38% of the block budget; the 105-track turn is 35k characters.
5. **Page, full**: done so far: chat with per-turn changes and undo, prompts, "Mix it", export (whole song, 24-bit WAV at
   the source rate, optional stems; checked: nulls against the raw sum to -113 dB with default settings, stems sum to the
   mix within -88 dB). Delivery loudness: a gain before the limiter, outside the mix settings, set by two
   corrections (checked: -14.08 LUFS by pyloudnorm, peaks at -1.0 dBFS). Also done: A/B against the agent's last turn, loop region by dragging on the timeline, rename
   (double-click) and regroup tracks (the group follows the name and is used by the engine and the prompt), the project
   file `mix_final_ACTUAL project.json` (autosaved every 10 s; reopened instead of a new first mix). Input is the folder
   the agent opens (no drag-and-drop: the agent picks the folder).
6. **Plugin packaging** (done, CI to be confirmed): skill `skills/mix/SKILL.md`; Claude Code manifest
   `.claude-plugin/plugin.json` (MCP server inline); Codex portable `plugin.json` + `mcp.json` (stdio, `./bin/...`);
   one marketplace file `.claude-plugin/marketplace.json` read by both (source: `url` + `ref: dist`, the source type both
   accept); `bin/mix-final-actual` picks the platform binary (Windows resolves `bin/mix-final-actual.exe`). CI builds
   every platform and force-pushes plugin files + binaries to the `dist` branch. Tested: Claude Code loads the plugin and
   Sonnet made a full 5-turn first mix through it (120 changes in turn 1, 10-21 in turns 2-5). OpenAI's public directory
   takes local-MCP plugins only from allowlisted partners; installing from this repo's marketplace needs no approval.
7. **Clean-up** (mostly done): `ReaperBridge`, VST3 hooks and host input buses removed. The APVTS stays (parameter
   storage, undo).
