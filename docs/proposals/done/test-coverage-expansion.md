# Proposal: Expand Test Coverage for Remaining Runtime Gaps

## Problem

The first big runtime-coverage wave has already landed. The repo now includes
direct tests for `server_dispatch`, `server_compute`, `mcp_server`, and
`trace_analysis`.

The remaining gaps are narrower but still important:

- no focused tests for CLI argument validation in the `cmd_*` family
- no direct test coverage for `client_integrations.c`
- limited direct coverage for work queue behavior in `cmd_work.c`
- new git/MCP surfaces need targeted tests as they stabilize

## Goals

- Extend the runtime test suite from the already-covered server/MCP surfaces to
  the remaining high-churn command and integration paths
- Keep shell scripts as smoke tests and add focused C tests for deterministic
  behavior

## Approach

### 1. Add direct `cmd_*` argv tests

Focus on:

- `cmd_core.c`
- `cmd_agent_trace.c`
- `cmd_work.c`
- `cmd_index.c`

### 2. Add client integration tests

Cover:

- Claude MCP registration merge behavior
- Codex plugin payload generation
- non-destructive updates to existing settings files

### 3. Add focused git/MCP tests

Extend the MCP suite for:

- `git_status`
- `git_commit`
- `git_push`
- `git_verify`

## Changes

| File | Change |
|------|--------|
| `src/tests/Rules.mk` | Add new focused runtime test targets |
| `src/tests/test_cmd_core.c` | New: top-level command argument handling |
| `src/tests/test_cmd_agent_trace.c` | New: delegate and queue parsing |
| `src/tests/test_cmd_work.c` | New: work queue command behavior |
| `src/tests/test_client_integrations.c` | New: settings/plugin generation behavior |
| `src/tests/test_mcp_git.c` | New: MCP git tool coverage |

## Acceptance Criteria

- [ ] High-traffic command handlers have direct argv tests
- [ ] Claude and Codex client integration paths have direct tests
- [ ] MCP git handlers have direct behavior coverage
- [ ] Existing shell smoke tests remain focused on packaging/end-to-end sanity
