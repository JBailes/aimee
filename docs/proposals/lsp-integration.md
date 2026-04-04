# Proposal: LSP integration for code intelligence

## Problem

aimee's code index (`index.c`, `extractors.c`) uses regex-based definition extraction. This works for symbol lookup and blast-radius analysis but cannot answer questions like "find all references to this function," "what's the type of this variable," or "rename this symbol across the workspace." oh-my-openagent integrates LSP (Language Server Protocol) for `lsp_rename`, `lsp_goto_definition`, `lsp_find_references`, and `lsp_diagnostics`, giving agents much richer code navigation.

For delegate agents editing code, the lack of LSP means they can't verify that their changes don't break references elsewhere, and they can't do safe renames. The webchat interface similarly lacks any code intelligence beyond text search.

## Goals

- Delegates and the CLI can query running LSP servers for goto-definition, find-references, and diagnostics.
- `aimee index` gains LSP-backed commands that supplement regex extraction.
- Webchat exposes code intelligence results in the chat UI (e.g., "show references" links).

## Approach

Rather than embedding language servers, aimee acts as an LSP client that connects to already-running language servers (or spawns them on demand from project config). This keeps aimee's footprint minimal.

### Architecture

```
aimee index lsp-refs <symbol> <file>
       |
       v
  LSP Client (src/lsp_client.c)
       |
       v
  Language Server (tsserver, gopls, clangd, etc.)
       |  (stdio or TCP)
       v
  JSON-RPC 2.0 responses
```

### New commands

| Command | LSP method | Purpose |
|---------|-----------|---------|
| `aimee index lsp-refs <file> <line> <col>` | `textDocument/references` | Find all references |
| `aimee index lsp-def <file> <line> <col>` | `textDocument/definition` | Goto definition |
| `aimee index lsp-rename <file> <line> <col> <new>` | `textDocument/rename` | Workspace rename |
| `aimee index lsp-diag [file]` | `textDocument/publishDiagnostics` | Get diagnostics/errors |

### LSP server config

In `.aimee/project.yaml` or `config.json`:

```yaml
lsp:
  typescript:
    command: ["typescript-language-server", "--stdio"]
    root_patterns: ["tsconfig.json", "package.json"]
  go:
    command: ["gopls", "serve"]
    root_patterns: ["go.mod"]
  c:
    command: ["clangd"]
    root_patterns: ["compile_commands.json", "Makefile"]
```

### Delegate tool integration

Add `lsp_references`, `lsp_definition`, `lsp_rename`, and `lsp_diagnostics` as delegate tools in `agent_tools.c`. Delegates can use these to verify changes and do safe renames.

### Webchat parity

The webchat chat API gains an `/api/lsp` endpoint. The frontend can request references/definitions for code blocks shown in chat, rendering them as clickable links. This is lower priority than the CLI/delegate integration.

### Changes

| File | Change |
|------|--------|
| `src/lsp_client.c` (new) | LSP client: initialize, send request, parse response, manage server lifecycle |
| `src/headers/lsp_client.h` (new) | LSP client types and API |
| `src/cmd_index.c` | Add `lsp-refs`, `lsp-def`, `lsp-rename`, `lsp-diag` subcommands |
| `src/agent_tools.c` | Add LSP tool handlers for delegates |
| `src/mcp_tools.c` | Expose LSP tools via MCP for primary agents |
| `src/webchat.c` | Add `/api/lsp` endpoint |
| `src/config.c` | Parse LSP server config |

## Acceptance Criteria

- [ ] `aimee index lsp-refs` returns all references for a symbol in a TypeScript project with tsserver
- [ ] `aimee index lsp-rename` applies a workspace-wide rename and reports changed files
- [ ] `aimee index lsp-diag` returns current diagnostics for a file
- [ ] Delegate agents can invoke `lsp_references` tool during execution
- [ ] LSP servers are spawned on first use and kept alive for the session, cleaned up on server shutdown
- [ ] `aimee --json index lsp-refs` returns structured JSON

## Owner and Effort

- **Owner:** aimee contributor
- **Effort:** L (5-7 days)
- **Dependencies:** Language servers must be installed on the host

## Rollout and Rollback

- **Rollout:** Feature is opt-in via LSP config. No LSP config = no LSP features, zero behavior change.
- **Rollback:** Revert commit. LSP server processes are cleaned up on aimee-server shutdown.
- **Blast radius:** Only affects users who configure LSP servers.

## Test Plan

- [ ] Unit tests: LSP JSON-RPC message construction and parsing
- [ ] Integration test: spawn a mock LSP server (simple echo), verify handshake and request/response
- [ ] Integration test: with `clangd` available, run `lsp-refs` on aimee's own codebase
- [ ] Manual verification: rename a Go function via `lsp-rename` in a test project

## Operational Impact

- **Metrics:** New counter `lsp_requests_total` by method and language.
- **Logging:** LSP server spawn/exit logged at INFO; individual requests at DEBUG.
- **Disk/CPU/Memory:** LSP servers can be memory-heavy (tsserver ~200MB). Documented as user responsibility.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| LSP integration | P3 | L | Medium -- major upgrade for code intelligence, but regex index works for most cases |

## Trade-offs

- **Alternative: embed tree-sitter.** Would give AST-level analysis without external servers, but doesn't provide type information, cross-file references, or rename refactoring. Could be a stepping stone.
- **Alternative: AST-grep (like OMO).** Pattern-aware search is powerful but doesn't replace LSP for type-aware operations. Could complement LSP as a separate proposal.
- **Limitation:** Requires language servers installed on the host. Documented as a prerequisite.
