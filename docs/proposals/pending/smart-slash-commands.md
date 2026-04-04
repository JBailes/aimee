# Proposal: Smart Slash Commands (/ultraplan, /bughunter, /teleport, /debug-tool-call)

## Problem

Aimee's command surface covers infrastructure management well but lacks higher-level agent workflow commands. Users manually compose multi-step prompts for deep planning, bug hunting, symbol navigation, and tool debugging.

The `soongenwong/claudecode` repo at `rust/crates/commands/src/lib.rs` implements 25+ slash commands organized by category, including several high-value workflow commands.

These work equally in CLI and webchat since they're structured prompts + tool chains.

## Goals

- `/ultraplan` produces a structured implementation plan before any code is written.
- `/bughunter` systematically scans a codebase and reports potential bugs with severity.
- `/teleport <symbol>` uses aimee's code index to jump to a definition.
- `/debug-tool-call` shows the last tool call's input, output, timing, and suggests fixes.
- All commands work in CLI and webchat sessions.

## Approach

### /ultraplan

Crafted prompt template: analyze request → list affected components → design ordered steps → identify risks → output structured markdown with checkboxes.

### /bughunter [path]

Crafted prompt template: scan path with Read/Grep/Glob → check for null dereferences, resource leaks, race conditions, error handling gaps, security issues → report as file:line + severity + description + fix.

### /teleport <symbol>

Wrapper around `aimee index find <symbol>` → read file at definition → display with context.

### /debug-tool-call

Retrieve from session state: last tool name, input, output, timing, error. Suggest: retry, check permissions, verify path, etc.

### Changes

| File | Change |
|------|--------|
| `src/cmd_core.c` | Register all four slash commands |
| `src/cmd_plan_deep.c` (new) | `/ultraplan` prompt template and output formatting |
| `src/cmd_bughunter.c` (new) | `/bughunter` scan prompt and finding aggregation |
| `src/cmd_teleport.c` (new) | `/teleport` index lookup + context display |
| `src/cmd_debug.c` (new) | `/debug-tool-call` last tool state retrieval |
| `src/server_session.c` | Store last tool call state for `/debug-tool-call` |
| `src/webchat.c` | Register commands in webchat command palette |

### Webchat Integration

All commands render as markdown in webchat. `/bughunter` renders as a sortable findings table. `/ultraplan` renders as a collapsible step list. Commands appear in the webchat command palette with descriptions.

## Acceptance Criteria

- [ ] `/ultraplan "add caching to the API layer"` produces a structured plan
- [ ] `/bughunter src/` reports findings with file:line references and severity
- [ ] `/teleport resolve_model_alias` shows the function with context
- [ ] `/debug-tool-call` after a failed Bash shows the error and suggestions
- [ ] All four work in CLI and webchat
- [ ] Commands listed in `/help`

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** M (2-3 days)
- **Dependencies:** `/teleport` requires code index. Others are standalone.

## Rollout and Rollback

- **Rollout:** Additive — new commands, no existing behavior changed.
- **Rollback:** Remove command registrations.
- **Blast radius:** None — user-initiated.

## Test Plan

- [ ] Unit tests: prompt template generation, argument parsing
- [ ] Integration tests: `/teleport` with a known indexed symbol
- [ ] Manual verification: run each command in CLI and webchat

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| `/ultraplan` | P2 | S | High — planning is critical |
| `/bughunter` | P3 | M | Medium — specialized but valuable |
| `/teleport` | P3 | S | Medium — thin wrapper |
| `/debug-tool-call` | P3 | S | Medium — debugging convenience |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/commands/src/lib.rs`.
