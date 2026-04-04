# Proposal: MCP Resources, Prompts, and SSE Transport

## Problem

The MCP server (`mcp_server.c`) implements only the `tools/*` capability surface:
- `initialize`, `tools/list`, `tools/call` (6 tools: search_memory, list_facts,
  get_host, list_hosts, find_symbol, delegate)

Missing from the MCP specification:
1. **`resources/*`:** No way for MCP clients to browse aimee's data (memories,
   indexes, configs) as structured resources.
2. **`prompts/*`:** No way to expose reusable workflows as composable prompt
   templates.
3. **Remote transport:** Current transport is stdio only — clients must be
   co-located processes. No HTTP SSE gateway for remote or browser-based MCP
   clients.

This limits interoperability with MCP-native IDEs, orchestrators, and hosted
workflows.

## Goals

- Implement `resources/list`, `resources/read` for memory and index data.
- Implement `prompts/list`, `prompts/get` for reusable workflows.
- Add optional HTTP SSE transport mode for remote clients.
- Pass MCP conformance tests for all implemented capabilities.

## Approach

### 1. Resources

Expose aimee data as MCP resources:

| URI | Description |
|-----|-------------|
| `aimee://memories/{tier}` | List memories in tier (L0-L3) |
| `aimee://memories/{id}` | Single memory with full content |
| `aimee://index/{project}` | Project index overview |
| `aimee://index/{project}/symbols` | Symbol table |
| `aimee://facts` | All stored facts |
| `aimee://config` | Current configuration (redacted secrets) |

Resource contents returned as `text/plain` or `application/json` MIME types.

### 2. Prompts

Expose workflow templates:

| Name | Description | Arguments |
|------|-------------|-----------|
| `search-and-summarize` | Search memories and summarize results | `query`, `tier` |
| `delegate-task` | Delegate a task to a sub-agent | `role`, `prompt`, `tools` |
| `review-changes` | Review git changes in a workspace | `workspace`, `scope` |
| `diagnose-issue` | Run diagnostic workflow | `description` |

Prompts return structured message arrays suitable for LLM consumption.

### 3. HTTP SSE transport

Add `aimee mcp --transport sse --port 8081` mode:

- HTTP endpoint accepts JSON-RPC requests via POST
- Responses streamed via Server-Sent Events
- Token-based authentication (reuse server auth token)
- Binds to `127.0.0.1` by default (explicit opt-in for LAN/remote)

**Platform note:** SSE transport uses standard HTTP — no platform-specific
dependencies. On Windows, the HTTP server uses the same portability layer
proposed in `windows-portability.md` (Winsock for sockets, no epoll dependency
in the HTTP path).

### 4. Capability advertisement

Update `initialize` response:
```json
{
    "capabilities": {
        "tools": {"listChanged": false},
        "resources": {"subscribe": false, "listChanged": false},
        "prompts": {"listChanged": false}
    },
    "serverInfo": {"name": "aimee", "version": "0.9.3"}
}
```

### Changes

| File | Change |
|------|--------|
| `src/mcp_server.c` | Add `resources/list`, `resources/read`, `prompts/list`, `prompts/get` handlers |
| `src/mcp_server.c` | Update `initialize` to advertise new capabilities |
| `src/mcp_sse.c` | New file: HTTP SSE transport layer |
| `src/mcp_sse.h` | New file: SSE transport API |
| `src/cmd_core.c` | Add `--transport` flag to `aimee mcp` subcommand |

## Acceptance Criteria

- [ ] `resources/list` returns all registered resource URIs
- [ ] `resources/read` returns resource content with correct MIME type
- [ ] `prompts/list` returns available prompt templates
- [ ] `prompts/get` returns structured message arrays with argument substitution
- [ ] SSE transport accepts JSON-RPC over HTTP POST, streams responses via SSE
- [ ] SSE transport requires authentication token
- [ ] SSE transport binds to `127.0.0.1` by default
- [ ] MCP conformance tests pass for tools + resources + prompts

## Owner and Effort

- **Owner:** TBD
- **Effort:** L
- **Dependencies:** None (benefits from structured error taxonomy for MCP error mapping)

## Rollout and Rollback

- **Rollout:** Resources and prompts activate in stdio mode immediately. SSE transport requires explicit `--transport sse` flag.
- **Rollback:** Revert commit. MCP returns to tools-only capability.
- **Blast radius:** Existing stdio MCP clients are unaffected (new capabilities are additive).

## Test Plan

- [ ] Unit test: resource URI routing returns correct data
- [ ] Unit test: prompt templates substitute arguments correctly
- [ ] Integration test: full MCP session with resources + prompts via stdio
- [ ] Integration test: SSE transport accepts and responds to JSON-RPC
- [ ] Conformance test: validate against MCP spec test suite
- [ ] Manual: connect external MCP client (e.g., Claude Desktop) over SSE

## Operational Impact

- **Metrics:** None.
- **Logging:** MCP method calls logged at DEBUG level.
- **Alerts:** None.
- **Disk/CPU/Memory:** SSE transport adds one HTTP listener. Memory usage proportional to connected clients.

## Priority

P1 — high ecosystem interoperability impact.

## Trade-offs

**Why not WebSocket instead of SSE?** SSE is simpler (one-directional streaming
over HTTP), aligns with MCP spec recommendations, and avoids WebSocket framing
complexity. The MCP spec explicitly supports SSE.

**Why not implement `resources/subscribe`?** Subscription requires change
detection and notification infrastructure. Starting without it keeps the
implementation simple; can be added later when use cases demand it.
