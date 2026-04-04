# Proposal: Split cmd_agent.c and cmd_agent_trace.c

## Problem

cmd_agent.c (1906 lines) is the second-largest file in the codebase. It mixes agent provider setup (codex, claude, gemini, openai, copilot), agent management (list, remove, network, tunnel), and token/OAuth handling. cmd_agent_trace.c (1350 lines) mixes delegation logic with job management, queue operations, context assembly, manifest handling, and trace viewing. Both files violate the Single Responsibility Principle and exceed the 1000-line target.

## Goals

- All resulting files under 1000 lines.
- Each file has a single, clear responsibility.
- No functional changes.

## Approach

### cmd_agent.c (1906 lines) splits into 3 files:

**cmd_agent.c (~500 lines):** Main dispatch (`cmd_agent`), list, network, tunnel, remove subcommands. The entry point and management operations stay together.

**cmd_agent_setup.c (~800 lines):** All provider setup functions: `setup_codex`, `setup_claude`, `setup_gemini`, `setup_openai`, `setup_copilot`. These are large, self-contained functions that each configure a specific provider.

**cmd_agent_auth.c (~400 lines):** Token management (`ag_token`), OAuth flow, refresh token handling. Auth is a cross-cutting concern that applies to multiple providers.

### cmd_agent_trace.c (1350 lines) splits into 2 files:

**cmd_delegate.c (~600 lines):** `cmd_delegate` and supporting functions: argument parsing, prompt preparation, background execution management, durable job tracking, verification.

**cmd_jobs.c (~500 lines):** `cmd_jobs`, `cmd_queue`, `cmd_context`, `cmd_manifest`, `cmd_trace`. These are all management/inspection commands for agent work products.

### Changes

| File | Change |
|------|--------|
| `src/cmd_agent.c` | Reduce to dispatch + management (~500 lines) |
| `src/cmd_agent_setup.c` | New: all provider setup functions |
| `src/cmd_agent_auth.c` | New: token and OAuth management |
| `src/cmd_delegate.c` | New: delegation logic (from cmd_agent_trace.c) |
| `src/cmd_jobs.c` | New: job/queue/trace management (from cmd_agent_trace.c) |
| `src/cmd_agent_trace.c` | Removed (contents split into cmd_delegate.c and cmd_jobs.c) |
| `src/headers/commands.h` | Update function declarations |
| `src/Makefile` | Replace cmd_agent_trace.o with cmd_delegate.o + cmd_jobs.o; add cmd_agent_setup.o + cmd_agent_auth.o |

## Acceptance Criteria

- [ ] cmd_agent.c is under 600 lines
- [ ] No resulting file exceeds 1000 lines
- [ ] cmd_agent_trace.c is removed (fully replaced)
- [ ] `make` builds clean with -Werror
- [ ] `make lint` passes
- [ ] All unit and integration tests pass unchanged
- [ ] `aimee agent`, `aimee delegate`, `aimee jobs` commands all work

## Owner and Effort

- **Owner:** TBD
- **Effort:** M (mechanical split, some static function visibility changes needed)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change, no behavior change.
- **Rollback:** git revert.
- **Blast radius:** Agent and delegation commands only.

## Test Plan

- [ ] Unit tests pass unchanged
- [ ] Integration tests pass unchanged
- [ ] Manual: `aimee agent list`, `aimee agent setup codex`, `aimee delegate` all work
- [ ] Manual: `aimee jobs`, `aimee queue`, `aimee trace` all work
- [ ] Build: no undefined symbol errors from the split

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** None. Pure structural change.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Split cmd_agent + cmd_agent_trace | P2 | M | Maintainability |

## Trade-offs

**Why split auth from setup?** Auth (OAuth, tokens) is reused across providers. Setup is provider-specific. Separating them makes it easier to add new providers without touching auth, and to modify auth without touching setup.

**Why rename cmd_agent_trace.c instead of keeping it?** The name "trace" is misleading since the file contains delegation, jobs, queue, and manifest handling. The split into cmd_delegate.c and cmd_jobs.c better reflects actual contents.
