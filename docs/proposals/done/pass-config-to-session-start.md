# Proposal: Pass Pre-loaded Config into cmd_session_start

## Problem

`main()` calls `config_load()` at `src/main.c:168`, then `cmd_session_start()` calls it again at `src/cmd_hooks.c:680`. The mtime cache prevents a full re-parse on the second call, but it still requires a `stat()` syscall, cache comparison, and `memcpy`. Similarly, `cmd_launch()` at `src/cmd_hooks.c:788` calls `config_load()` a third time after `cmd_session_start()` returns.

## Goals

- Eliminate redundant `config_load()` calls on the startup path.
- Config is loaded exactly once per process.

## Approach

Add a `config_t *cfg` pointer to `app_ctx_t`. When `main()` loads config, it stashes a pointer in `ctx.cfg`. `cmd_session_start()` and `cmd_launch()` use `ctx->cfg` when available, falling back to `config_load()` when called via the subcommand dispatch table (where `ctx->cfg` may be NULL).

### Changes

| File | Change |
|------|--------|
| `src/headers/aimee.h` | Add `config_t *cfg` field to `app_ctx_t` |
| `src/main.c` | Set `ctx.cfg = &cfg` after `config_load()` |
| `src/cmd_hooks.c` | In `cmd_session_start()` and `cmd_launch()`: use `ctx->cfg` if non-NULL, else call `config_load()` |

## Acceptance Criteria

- [ ] `config_load()` called exactly once during `aimee` (no-subcommand) startup
- [ ] `aimee session-start` (subcommand path) still works (falls back to own `config_load`)
- [ ] All existing tests pass

## Owner and Effort

- **Owner:** JBailes
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change.
- **Rollback:** `git revert`.
- **Blast radius:** Minimal — config is the same data, just loaded fewer times.

## Test Plan

- [ ] Integration tests: `aimee` startup works identically
- [ ] Integration tests: `aimee session-start` (direct subcommand) works
- [ ] Manual verification: strace shows single `open()` of config.json on startup

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** Saves 1-2 stat + memcpy cycles per startup.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Pass config to session-start | P1 | S | Saves ~1-2ms, cleaner architecture |

## Trade-offs

Adds a pointer field to `app_ctx_t`, but this is cleaner than the implicit re-load pattern. The fallback path (NULL cfg pointer) ensures backward compatibility when `cmd_session_start` is called as a standalone subcommand.
