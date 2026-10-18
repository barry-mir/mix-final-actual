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
3. **Session maker in C++**: excerpt choice, loudness gain, silent-track removal, group guess from names (now
   Python in the research repo).
4. **No track limit**: replace the 64-bit `TrackMask` and fixed arrays with sizes set when a session loads.
5. **Page, full**: chat and change history with undo, A/B of a turn, loop region as the analysis window, rename and
   regroup tracks, export, project save/open, drag-and-drop input.
6. **Plugin packaging**: skill (`SKILL.md`), Claude Code and Codex manifests, marketplace file, binary download on
   first run; CI builds for macOS, Windows and Linux.
7. **Clean-up**: drop the plugin-host leftovers (`ReaperBridge`, VST3 extensions, the APVTS where it only served
   hosts).
