# Proposal: CLI Startup Improvements

## Problem

Two issues slow down or waste work during CLI startup:

1. **Well-known socket timeout too high (1000ms).** In `cli_ensure_server`, the well-known socket probe uses `CLIENT_CONNECT_TIMEOUT_MS` (1000ms). A healthy local Unix socket connects in <1ms. If the socket file exists but the server is wedged, this wastes a full second before falling back to spawning a new server.

2. **Prune child runs full schema migrations.** In `cmd_post_checkout`, the forked prune child calls `db_open()` which runs full schema migrations. The parent already called `db_open()` moments earlier, so migrations are guaranteed current. The child should use `db_open_fast()` to skip redundant migration checks.

## Goals

- Reduce worst-case well-known socket probe time from 1000ms to 200ms.
- Eliminate redundant schema migration work in the prune child process.

## Approach

### Changes

| File | Change |
|------|--------|
| `src/cli_client.c` | Replace `CLIENT_CONNECT_TIMEOUT_MS` with `200` in the well-known socket `try_server` call |
| `src/cmd_hooks.c` | Replace `db_open()` with `db_open_fast()` in the prune child fork block |

## Acceptance Criteria

- [ ] Well-known socket probe times out after 200ms instead of 1000ms
- [ ] Prune child opens database without running schema migrations
- [ ] No change to behavior when server is healthy (connects in <1ms either way)
- [ ] Project compiles cleanly and passes lint

## Owner and Effort

- **Owner:** TBD
- **Effort:** XS
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Immediate on next build. No configuration changes needed.
- **Rollback:** Revert commit. Restores 1000ms timeout and full migrations in prune child.
- **Blast radius:** Minimal. The 200ms timeout is still 200x longer than a healthy Unix socket connect. The prune child's migration skip is safe because the parent already ran migrations.

## Test Plan

- [ ] Manual: verify `aimee` CLI starts correctly with a running persistent server
- [ ] Manual: verify `aimee` CLI starts correctly when no persistent server is running
- [ ] Manual: verify prune still runs successfully after post-checkout hook

## Operational Impact

- **Metrics:** None new.
- **Logging:** None new.
- **Alerts:** None.
- **Disk/CPU/Memory:** Saves up to 800ms wall-clock time on wedged-server path. Saves one migration check per post-checkout hook invocation.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Socket timeout reduction | P2 | XS | Saves up to 800ms on failure path |
| Fast db open in prune child | P2 | XS | Eliminates redundant migration work |
