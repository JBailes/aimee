# Proposal: MCP Proxy — Resilient MCP via Client-Side Stdio Bridge

## Problem

The `aimee-mcp` binary runs as a standalone stdio process launched by Claude Code.
When it crashes, times out, or is restarted by the MCP lifecycle, Claude Code sees
all aimee tools vanish and reappear. This is disruptive: the model loses tool
definitions mid-conversation, deferred tool searches fail, and the user has to
manually `/mcp` reconnect.

The root cause is that `aimee-mcp` is monolithic — it owns both the MCP protocol
layer (stdio JSON-RPC) and all business logic (DB queries, delegate dispatch, git
operations). Any failure in the business logic kills the entire MCP session.

## Proposed Architecture

Merge MCP serving into the `aimee` client binary and make it a thin proxy to
`aimee-server`:

```
Before:
  Claude Code  --stdio-->  aimee-mcp  --sqlite-->  DB
  User         --cli--->   aimee      --unix socket-->  aimee-server

After:
  Claude Code  --stdio-->  aimee mcp-serve  --unix socket-->  aimee-server
  User         --cli--->   aimee            --unix socket-->  aimee-server
```

`aimee mcp-serve` is a new subcommand on the existing `aimee` client binary:
1. Accepts MCP JSON-RPC on stdio (same interface Claude Code expects)
2. Translates `tools/call` requests into server RPC calls over the Unix socket
3. Translates server responses back into MCP JSON-RPC results
4. Handles `initialize`, `tools/list`, and `prompts/*` locally (no server round-trip)

The separate `aimee-mcp` binary is retired. `client_integrations.c` is updated
to point Claude Code at `aimee mcp-serve` instead.

The `aimee-server` gains a new dispatch method `mcp.call` that handles
tool execution. This is trivially mapped from the existing `handle_*` functions
that `mcp_server.c` already calls.

## Why This Works

- **One binary.** The client already knows how to connect to the server, auto-launch
  it, and reconnect. The MCP proxy is just another mode of the same binary.
- **The proxy layer is trivially simple.** It's a stdio-to-socket bridge with a
  static tool list. There's almost no logic to crash.
- **Server disconnects are invisible.** If the server restarts, the proxy reconnects
  on the next request. Claude Code never sees a tool disappear.
- **Request buffering.** The proxy can hold a request for a few seconds while
  waiting for the server to come back, rather than returning an error immediately.
- **The server already handles everything.** The dispatch table in `server.c`
  already has `memory.search`, `delegate`, `index.find`, `attempt.record`, etc.
  The git tools call CLI commands that work the same way. No logic duplication.
- **Simpler build.** One fewer binary to compile, link, install, and maintain.
  The Makefile `mcp` target and its separate link step go away.

## Design

### `aimee mcp-serve` subcommand

Added to the client's command table in `cli_main.c`. Entry point reads stdio
and forwards to the server:

```
handle_tool_call(tool_name, args):
    method = mcp_to_server_method(tool_name)  // e.g. "search_memory" -> "memory.search"
    conn = get_or_reconnect_server()
    response = conn.send_rpc(method, args)
    return format_mcp_result(response)
```

Static data served locally without server round-trip:
- `initialize` response (capabilities, protocol version)
- `tools/list` response (tool definitions — compiled into the binary)
- `prompts/list` and `prompts/get` responses

Connection management:
- Connect to server on startup (auto-launch if needed, same as CLI commands)
- Auto-reconnect on send failure (up to 3 retries, 500ms apart)
- If server is down after retries, return MCP error with helpful message
  ("aimee server unavailable, run `aimee` to restart")

### Server (new `mcp.call` method)

Add a single new method to the server dispatch table:

```c
{"mcp.call", handle_mcp_call},
```

`handle_mcp_call` receives `{"tool": "search_memory", "arguments": {...}}` and
dispatches to the same handler functions the current `mcp_server.c` uses, but
through the server's connection/DB infrastructure instead of direct sqlite access.

The existing handlers in `mcp_server.c` (e.g., `handle_search_memory`,
`handle_delegate`, `handle_git_status`) become shared code linked into the
server library.

### Tool-to-Method Mapping

| MCP Tool | Server Method | Notes |
|----------|---------------|-------|
| `search_memory` | `memory.search` | Already exists |
| `list_facts` | `memory.list` | Already exists (filter tier=L2) |
| `get_host` | Server-side new handler | Reads agents.json network |
| `list_hosts` | Server-side new handler | Reads agents.json network |
| `find_symbol` | `index.find` | Already exists |
| `delegate` | `delegate` | Already exists |
| `delegate_reply` | `delegate.reply` | Already exists |
| `preview_blast_radius` | `blast_radius.preview` | Already exists |
| `record_attempt` | `attempt.record` | Already exists |
| `list_attempts` | `attempt.list` | Already exists |
| `git_*` (13 tools) | `mcp.git.*` or `cli.forward` | New; calls same git helpers |

### Git Tools

The 13 git tools (`git_status`, `git_commit`, `git_push`, etc.) currently shell
out to git/gh commands in `mcp_git.c`. Two options:

**Option A: Forward as `cli.forward`**
The proxy sends the tool name and args; the server execs the git command and
returns stdout. Simple, reuses existing `cli.forward` infrastructure.

**Option B: New `mcp.git.*` methods**
The server gets dedicated handlers for each git operation. More structured,
better error handling, but more code.

Recommend **Option A** for initial implementation, graduating to B if needed.

## Migration

1. `client_integrations.c` is updated to configure Claude Code with
   `"command": "/usr/local/bin/aimee", "args": ["mcp-serve"]` instead of
   pointing to `aimee-mcp`.
2. The `aimee-mcp` binary is removed from `Makefile` and `make install`.
3. Existing installations auto-migrate on next `aimee init` / session-start
   (the integration setup runs every time and will update the settings).

## Implementation Order

1. Add `mcp.call` dispatch to `aimee-server` for the non-git tools
   (memory, index, delegate, attempts, blast radius, hosts)
2. Add `mcp-serve` subcommand to `aimee` client that does stdio JSON-RPC
   and forwards `tools/call` to server via Unix socket
3. Add reconnection logic (reuse existing `cli_connect_timeout` with retry)
4. Add git tool forwarding (Option A)
5. Update `client_integrations.c` to point at `aimee mcp-serve`
6. Test: kill server mid-session, verify proxy reconnects transparently
7. Remove `aimee-mcp` binary from Makefile, install targets, and CI

## Files Changed

| File | Change |
|------|--------|
| `cli_main.c` | Add `mcp-serve` to command table |
| `cli_mcp_serve.c` (new) | Stdio JSON-RPC loop, tool forwarding |
| `server.c` | Add `mcp.call` to dispatch table |
| `server_mcp.c` (new) | Implement `handle_mcp_call` dispatcher |
| `client_integrations.c` | Point Claude Code at `aimee mcp-serve` |
| `Makefile` | Remove `aimee-mcp` target, add `cli_mcp_serve.c` to CLI sources |
| `mcp_server.c` | Retire (extract reusable parts to `server_mcp.c`) |
| `mcp_git.c` | No change (linked into server for git forwarding) |

## Risks

- **Latency**: Adds one Unix socket round-trip per tool call (~0.1ms). Negligible.
- **Server dependency**: If server is truly dead, tools fail. Mitigated by
  auto-reconnect and the client's existing server auto-launch logic.
- **Git cwd bug**: The current bug where git tools run from the wrong directory
  exists in both architectures. This proposal doesn't fix it but doesn't make it
  worse. (That's a separate issue in how `mcp_git.c` resolves the working directory.)
