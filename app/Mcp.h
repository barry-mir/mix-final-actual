#pragma once
// `mix-final-actual mcp`: an MCP server (JSON-RPC 2.0, one message per line on stdin/stdout) that a coding agent
// starts. It is a thin front: the mixing runs in a long-lived `serve` instance (started detached on first use, so the
// page keeps playing when the agent session ends); these tools fetch that instance's turns and send the answers.
namespace mfa
{
int runMcp (const char* argv0);
}
