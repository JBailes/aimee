# Proposal: Split cmd_hooks.c, cmd_chat.c, and cli_client.c

## Problem

cmd_hooks.c (1017 lines) mixes tool hook dispatch, session lifecycle management, and context building. cmd_chat.c (1239 lines) mixes provider abstraction, streaming, tool execution, and the main chat loop. cli_client.c (1055 lines) mixes connection management, server spawn, and RPC marshaling. All three exceed the 1000-line target and violate Single Responsibility.

## Goals

- All resulting files under 1000 lines.
- Single responsibility per file.
- No functional changes.

## Approach

### cmd_hooks.c (1017 lines) splits into 3 files:

**cmd_hooks.c (~400 lines):** `cmd_hooks` (pre/post tool dispatch), `cmd_launch`, delegation detection logic.

**cmd_session.c (~350 lines):** `cmd_session_start`, `cmd_wrapup`, `cleanup_worktrees`, `prune_stale_sessions`, `remove_stale_worktrees`. Session lifecycle is self-contained.

**cmd_session_context.c (~270 lines):** `build_session_context`, `build_capabilities_text`. Context assembly is complex but only called at session start.

### cmd_chat.c (1239 lines) splits into 2 files:

**cmd_chat.c (~600 lines):** `cmd_chat` entry point, main message loop, provider initialization, tool setup, system prompt building.

**cmd_chat_stream.c (~640 lines):** Provider-specific send/parse functions (openai, anthropic), `chat_via_claude` (Claude CLI forwarding), SSE event parsing, streaming response handling.

### cli_client.c (1055 lines) splits into 3 files:

**cli_client.c (~400 lines):** Connection management, authentication, core request/response cycle.

**cli_server_spawn.c (~300 lines):** `spawn_server`, `wait_for_ready`, lock acquisition, socket cleanup. Server lifecycle is independent of request handling.

**cli_rpc.c (~355 lines):** RPC route lookup, request marshaling for all command methods, text output formatting.

### Changes

| File | Change |
|------|--------|
| `src/cmd_hooks.c` | Reduce to tool hooks + launch (~400 lines) |
| `src/cmd_session.c` | New: session lifecycle |
| `src/cmd_session_context.c` | New: context builders |
| `src/cmd_chat.c` | Reduce to main loop (~600 lines) |
| `src/cmd_chat_stream.c` | New: provider streaming |
| `src/cli_client.c` | Reduce to core client (~400 lines) |
| `src/cli_server_spawn.c` | New: server spawn logic |
| `src/cli_rpc.c` | New: RPC marshaling |
| `src/headers/commands.h` | Update declarations |
| `src/headers/cli_client.h` | Update declarations |
| `src/Makefile` | Add new .o files |

## Acceptance Criteria

- [ ] cmd_hooks.c is under 500 lines
- [ ] No resulting file exceeds 1000 lines
- [ ] `make` builds clean with -Werror
- [ ] `make lint` passes
- [ ] All unit and integration tests pass unchanged
- [ ] `aimee session-start`, `aimee hooks`, `aimee wrapup` all work
- [ ] `aimee chat` works (if applicable to test environment)

## Owner and Effort

- **Owner:** TBD
- **Effort:** M (mechanical split, visibility changes for static functions)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change, no behavior change.
- **Rollback:** git revert.
- **Blast radius:** CLI, session management, chat. Core functionality.

## Test Plan

- [ ] Unit tests pass (especially test_cli_launch)
- [ ] Integration tests pass unchanged
- [ ] Manual: `aimee session-start` outputs context correctly
- [ ] Manual: hooks pre/post work for tool calls
- [ ] Manual: server auto-spawn works from CLI

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** None. Pure structural change.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Split hooks + chat + cli | P2 | M | Maintainability |

## Trade-offs

**Why keep cmd_chat.c at 600 lines instead of splitting further?** The main loop, provider init, and tool setup are tightly coupled. Splitting them would create artificial boundaries with heavy parameter passing.

**Why three files for cli_client.c?** Server spawn is independently testable and changes rarely. RPC marshaling changes whenever commands are added. The core client is stable. Three files reflect three rates of change.
