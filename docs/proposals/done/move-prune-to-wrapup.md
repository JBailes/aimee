# Proposal: Move Stale Session Pruning from Startup to Wrapup

## Problem

`cmd_session_start()` forks a child to run `prune_stale_sessions()`, which calls `git worktree remove --force` on old session worktrees. Although the fork is non-blocking (`WNOHANG`), the child holds git's worktree lock, contending with the new session's `worktree_ensure()` threads that need `git worktree add`. This means old sessions slow down new session startup.

Observable: startup latency scales with stale session count. New sessions should not pay cleanup costs for old sessions.

## Goals

- New session startup is not impacted by old session state.
- Stale sessions are still pruned (just at a different time).

## Approach

Move the `prune_stale_sessions()` fork from `cmd_session_start()` to `cmd_wrapup()`. The exiting session cleans up other stale sessions on its way out, when there's no contention with worktree creation.

### Changes

| File | Change |
|------|--------|
| `src/cmd_hooks.c` | Remove prune fork from `cmd_session_start()`; add it to `cmd_wrapup()` after own cleanup |

## Acceptance Criteria

- [ ] `cmd_session_start()` does not call `prune_stale_sessions()`
- [ ] `cmd_wrapup()` prunes stale sessions in a background fork after own cleanup
- [ ] Project compiles cleanly with `-Werror`
- [ ] New session startup no longer contends on git worktree lock with old session cleanup

## Owner and Effort

- **Owner:** TBD
- **Effort:** XS
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Immediate on next build.
- **Rollback:** Revert commit. Moves prune back to session-start.
- **Blast radius:** Minimal. Stale sessions accumulate slightly longer (until next wrapup instead of next startup). Crashed sessions that never wrapup are cleaned up by the next session that does wrapup.

## Test Plan

- [ ] Manual: start new session, verify no git lock contention during worktree creation
- [ ] Manual: exit session, verify stale sessions are pruned during wrapup
- [ ] Manual: verify stale worktrees from crashed sessions are cleaned up on next normal wrapup

## Operational Impact

- **Metrics:** None new.
- **Logging:** No change.
- **Alerts:** None.
- **Disk/CPU/Memory:** Stale worktrees may persist slightly longer between sessions. Negligible impact.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Move prune to wrapup | P1 | XS | Eliminates git lock contention on startup |
