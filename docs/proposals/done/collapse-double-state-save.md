# Proposal: Collapse Double session_state_force_save

## Problem

`cmd_session_start()` calls `session_state_force_save()` twice:

1. At `src/cmd_hooks.c:693` — writes a placeholder state file before forking the prune child, so the prune child's orphan-worktree check won't delete the new session directory.
2. At `src/cmd_hooks.c:755` — writes the state again after worktree paths and fetched_mask are populated.

Both writes go to the same file with `fsync` semantics. The second write completely overwrites the first.

## Goals

- Reduce `session_state_force_save()` calls from 2 to 1 on the startup path.
- Maintain correctness: prune child must not delete the new session's directory.

## Approach

Move worktree path computation (the `for` loop at lines 714-728) and fetched_mask computation (lines 737-752) before the prune fork. This way a single `session_state_force_save()` writes both the placeholder and the complete worktree state before the fork. The prune child sees the full state.

The key insight is that worktree path computation is pure string formatting (no I/O) — `snprintf` calls that build paths from config data. There's no reason it must happen after the fork.

### Changes

| File | Change |
|------|--------|
| `src/cmd_hooks.c` | In `cmd_session_start()`: move worktree path loop and fetched_mask computation before the fork; remove the first `session_state_force_save()` call |

## Acceptance Criteria

- [ ] `session_state_force_save()` called exactly once in `cmd_session_start()`
- [ ] Prune child still sees a valid state file for the current session
- [ ] Worktree paths are correctly populated before being written
- [ ] All existing tests pass

## Owner and Effort

- **Owner:** JBailes
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change.
- **Rollback:** `git revert`.
- **Blast radius:** Session startup only.

## Test Plan

- [ ] Integration tests: `aimee session-start` produces correct worktree paths
- [ ] Integration tests: stale session pruning still works
- [ ] Manual verification: strace shows single write to session state file

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** Saves one fsync'd write (~1ms on SSD, more on spinning disk).

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Collapse double state save | P1 | S | Saves ~1-2ms per startup |

## Trade-offs

None significant. The worktree path computation is pure string formatting that doesn't depend on anything set up by the prune child. Moving it earlier is strictly correct.
