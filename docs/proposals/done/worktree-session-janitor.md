# Proposal: Worktree and Session Janitor Service

## Problem

Worktrees are created lazily per session (`worktree_ensure()`, `guardrails.h:65`)
with a max of 16 per session (`MAX_WORKTREES = 16`). However:

1. **No automatic cleanup.** Stale worktrees from crashed or abandoned sessions
   persist on disk indefinitely. Each worktree is a near-full copy of a
   repository, consuming significant disk space.
2. **No age policy.** There is no mechanism to identify which worktrees are stale
   vs actively in use.
3. **No disk budget.** Worktree accumulation has no ceiling — a long-running
   deployment with many sessions will eventually exhaust disk.
4. **No audit trail.** Worktree creation/deletion events are not logged, making
   it hard to diagnose disk usage growth.

## Goals

- Stale worktrees are cleaned up automatically based on age policy.
- Disk budget prevents unbounded growth.
- Cleanup is safe — active sessions are never cleaned.
- Audit log tracks creation and deletion events.

## Approach

### 1. Worktree registry

Extend the database with a `worktrees` table:

```sql
CREATE TABLE IF NOT EXISTS worktrees (
    id INTEGER PRIMARY KEY,
    session_id TEXT NOT NULL,
    workspace TEXT NOT NULL,
    path TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    last_accessed_at INTEGER NOT NULL,
    size_bytes INTEGER DEFAULT 0,
    state TEXT NOT NULL DEFAULT 'active'  -- active, stale, deleted
);
```

`worktree_ensure()` inserts a row on creation. Access updates `last_accessed_at`.

### 2. Staleness detection

A worktree is stale when:
- Its session has ended (no active session with that `session_id`)
- AND `last_accessed_at` is older than the age threshold (default: 24 hours)

Active sessions are never considered stale, regardless of age.

### 3. Janitor command

`aimee worktree gc`:
1. Query for stale worktrees
2. For each: verify no active session, then `git worktree remove --force`
3. Mark row as `deleted` (soft delete for audit)
4. Report: removed N worktrees, freed M bytes

### 4. Automatic janitor

Run janitor logic during `session-start` (in the background fork, similar to
existing prune child in `cmd_hooks.c`). Only run if last janitor run was >6
hours ago (prevent repeated work on rapid session cycling).

### 5. Disk budget

Add configurable disk budget (default: 10GB). When total worktree size exceeds
budget, clean stale worktrees oldest-first until under budget. If still over
budget after cleaning all stale worktrees, warn but do not delete active ones.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Add `worktrees` table migration |
| `src/guardrails.c` | Register worktree creation in DB, update access timestamps |
| `src/cmd_hooks.c` | Run janitor in background during `session-start` |
| `src/cmd_core.c` | Add `aimee worktree gc` subcommand |
| `src/cmd_table.c` | Register subcommand |

## Acceptance Criteria

- [ ] Worktree creation/access tracked in database with timestamps
- [ ] `aimee worktree gc` removes stale worktrees and reports freed space
- [ ] Active session worktrees are never cleaned
- [ ] Automatic janitor runs during `session-start` when >6 hours since last run
- [ ] Disk budget triggers cleanup when exceeded
- [ ] Soft-delete audit trail preserved in database

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Database migration adds table. Janitor activates on next `session-start`.
- **Rollback:** Revert commit. Existing worktrees remain on disk (manual cleanup).
- **Blast radius:** If staleness detection has bugs, active worktrees could be cleaned. The active-session check is the safety net.

## Test Plan

- [ ] Unit test: staleness detection correctly identifies ended sessions
- [ ] Unit test: active sessions are never marked stale
- [ ] Integration test: create worktree, end session, run gc, verify removal
- [ ] Integration test: disk budget triggers oldest-first cleanup
- [ ] Failure injection: concurrent session-start and gc — verify no race

## Operational Impact

- **Metrics:** Worktree count, total size, last gc run timestamp.
- **Logging:** Janitor logs worktrees removed and bytes freed.
- **Alerts:** None.
- **Disk/CPU/Memory:** Reduces disk usage by cleaning stale worktrees. Janitor adds ~100ms to session-start (background, non-blocking).

## Priority

P2 — prevents disk bloat in long-lived deployments.

## Trade-offs

**Why 24-hour default age?** Short enough to prevent accumulation, long enough
that a user who resumes a session the next day still has their worktree. Users
who need longer can configure the threshold.

**Why soft-delete instead of hard-delete?** Audit trail for diagnosing disk usage
patterns. Deleted rows can be purged after 30 days.
