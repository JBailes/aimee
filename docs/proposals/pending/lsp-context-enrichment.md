# Proposal: LSP Context Enrichment for Agent Tool Calls

## Problem

When aimee delegates code-editing tasks to agents (Claude Code, codex, ollama), those agents operate without LSP awareness. They cannot see workspace diagnostics, go-to-definition results, or reference locations before or after making changes. This means:

1. Agents make edits that introduce type errors, unused imports, or broken references — only caught later by manual review or CI.
2. Agents cannot navigate symbol definitions across files without grep/glob heuristics, which are slow and imprecise compared to LSP.
3. Post-edit verification relies on full `cargo check` / `go vet` / `gcc` cycles rather than incremental LSP diagnostics.

The `soongenwong/claudecode` Rust port at `rust/crates/lsp/` implements a working LSP manager that:
- Spawns and manages LSP server processes per file extension (`lsp/src/manager.rs`)
- Collects workspace diagnostics asynchronously (`WorkspaceDiagnostics`, `FileDiagnostics`)
- Provides go-to-definition and find-references via standard LSP protocol (`lsp/src/client.rs`)
- Renders all of the above into a prompt-injectable `LspContextEnrichment` section (`lsp/src/types.rs`)
- Has full test coverage with a mock Python LSP server

This is directly usable as a library or as an aimee MCP tool surface.

## Goals

- Aimee agents receive LSP diagnostics, definitions, and references as structured context before and after edits.
- Agents can verify their own edits produce zero new diagnostics without running full builds.
- The feature works across all primary agents (Claude Code via hooks/MCP, codex via delegate, ollama via delegate).

## Approach

Add an `lsp` subsystem to aimee that manages LSP server lifecycles per workspace. Expose it through two surfaces:

1. **MCP tool**: `lsp_diagnostics`, `lsp_definition`, `lsp_references` — callable by any MCP-connected agent.
2. **Post-tool-use hook enrichment**: After `Edit`, `Write`, or `Bash` tool calls that touch source files, automatically sync the changed file to the LSP and inject fresh diagnostics into the agent's context.

### Architecture

The `soongenwong/claudecode` LSP crate (`rust/crates/lsp/`) is written in Rust with tokio async. Since aimee is C-based, we have two viable paths:

**Option A — C reimplementation using aimee's existing patterns:**
- Implement LSP JSON-RPC client in C using aimee's existing `platform_process.c` (subprocess management) and `platform_ipc.c` (IPC/pipe handling).
- Store diagnostics in aimee's existing SQLite DB or in-memory working memory.
- Expose via existing MCP server (`mcp_server.c`, `mcp_tools.c`).

**Option B — Rust FFI bridge:**
- Compile the `lsp` crate as a `cdylib` with a thin C-ABI wrapper.
- Link into aimee at build time.
- Lower integration effort but adds Rust toolchain as build dependency.

Recommend **Option A** — it stays within aimee's single-toolchain philosophy and the LSP JSON-RPC protocol is straightforward to implement in C (it's just `Content-Length` framed JSON over stdio pipes).

### Changes

| File | Change |
|------|--------|
| `src/lsp_client.c` (new) | LSP JSON-RPC stdio client: initialize, didOpen, didChange, didSave, definition, references, publishDiagnostics handler |
| `src/lsp_manager.c` (new) | Per-workspace LSP server lifecycle management, extension-to-server mapping |
| `src/mcp_tools.c` | Add `lsp_diagnostics`, `lsp_definition`, `lsp_references` tool handlers |
| `src/cmd_hooks.c` | Add post-Edit/Write LSP sync and diagnostic injection |
| `src/config.c` | Add `lsp_servers` config section (server name, command, args, extensions) |
| `src/headers/lsp.h` (new) | Public API for LSP subsystem |

### LSP Server Config Format

```json
{
  "lsp_servers": [
    {
      "name": "rust-analyzer",
      "command": "rust-analyzer",
      "args": [],
      "extensions": [".rs"],
      "workspace_root": "auto"
    },
    {
      "name": "gopls",
      "command": "gopls",
      "args": ["serve"],
      "extensions": [".go"]
    }
  ]
}
```

### Prompt Rendering Format

When injecting LSP context into agent prompts, use a structured markdown format (validated by claw-code's `lsp/types.rs`):

```markdown
# LSP context
Focus: src/memory.c — 3 diagnostics across 2 files

Diagnostics (showing 3 of 3):
 - src/memory.c:42:5 [error] implicit declaration of function 'free_tier'
 - src/memory.c:87:12 [warning] unused variable 'tmp'
 - src/config.c:15:1 [warning] missing include guard

Definitions (showing 2 of 2):
 - free_tier: src/memory_promote.c:23
 - config_load: src/config.c:45

References (showing 3 of 8):
 - free_tier: src/agent.c:112, src/tasks.c:55, src/server.c:201
 (5 more omitted for brevity)
```

Key formatting rules:
- Cap at 12 diagnostics and 12 definition/reference entries to control token budget
- Severity labels: "error", "warning", "info", "hint"
- Replace newlines in diagnostic messages with spaces
- Include file:line:column coordinates for precise navigation

### Webchat and Dashboard Integration

- **Webchat**: Expose a `/api/lsp/diagnostics` endpoint; render diagnostic counts as a badge in the chat header (e.g., "2 errors, 1 warning")
- **Dashboard**: Add an "LSP Health" card showing active servers, diagnostic counts per workspace, and server restart events

## Acceptance Criteria

- [ ] `aimee mcp` exposes `lsp_diagnostics` tool returning structured diagnostics for a workspace
- [ ] `aimee mcp` exposes `lsp_definition` tool returning file:line for a symbol at a given position
- [ ] `aimee mcp` exposes `lsp_references` tool returning all reference locations for a symbol
- [ ] Post-edit hook automatically reports new diagnostics to the agent context
- [ ] Works with at least rust-analyzer and gopls
- [ ] LSP servers are started lazily (on first file touch) and shut down cleanly on session end
- [ ] Prompt rendering caps at 12 diagnostics and 12 symbol entries
- [ ] **Webchat**: diagnostic counts visible as badge in chat header
- [ ] **Dashboard**: LSP health card shows active servers and diagnostic counts

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** L (3-5 days)
- **Dependencies:** None — new subsystem, no conflicts with pending proposals

## Rollout and Rollback

- **Rollout:** Config-gated — LSP enrichment only activates when `lsp_servers` config is present. Zero behavior change for existing users.
- **Rollback:** Remove config section. LSP processes are ephemeral.
- **Blast radius:** Only affects sessions where LSP is configured. Failure mode is graceful — if LSP server crashes, tools return empty results and agents fall back to grep/glob.

## Test Plan

- [ ] Unit tests: JSON-RPC framing, didOpen/didChange lifecycle, diagnostic parsing
- [ ] Integration tests: Mock LSP server (Python script from reference impl) validates full request/response cycle
- [ ] Failure injection: LSP server crash mid-session, invalid JSON responses, server startup timeout
- [ ] Manual verification: Edit a `.rs` file via delegate, confirm diagnostics appear in agent context

## Operational Impact

- **Metrics:** `lsp_requests_total`, `lsp_diagnostics_count`, `lsp_server_restarts`
- **Logging:** LSP server spawn/shutdown logged at INFO, protocol errors at WARN
- **Alerts:** None — LSP failure is non-fatal
- **Disk/CPU/Memory:** One LSP server process per language per workspace. rust-analyzer uses ~200MB RSS for medium projects. Servers are idle except during edits.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| LSP client + manager | P2 | L | High — eliminates entire class of agent-introduced errors |
| MCP tool surface | P2 | S | High — makes it usable by all agents |
| Post-edit hook integration | P3 | M | Medium — quality-of-life improvement |

## Trade-offs

- **Why not just use `cargo check`?** LSP provides incremental, per-file diagnostics in <100ms vs full build in 5-30s. It also provides navigation (definitions, references) that builds don't.
- **Why not Option B (Rust FFI)?** Adds Rust toolchain as mandatory build dependency. aimee is a C project — keeping it pure C avoids build complexity and makes it easier for delegates to contribute.
- **Why not a standalone sidecar?** An MCP-native LSP bridge would work but adds deployment complexity. Embedding in aimee keeps the single-binary advantage.

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/lsp/` — fully tested async LSP manager with mock server, diagnostics collection, go-to-definition, find-references, and prompt context rendering.
