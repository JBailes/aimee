# Proposal: Work Queue Audit Trail and Result Tracking

## Problem

The work queue has no history of state transitions. When items move between
statuses (pending, claimed, done, failed), only the current state is stored.
This causes several issues:

1. **Items vanish without explanation.** During a queue processing session,
   the queue went from 36 items to 15 with 0 done and 0 failed. There is no
   way to determine what happened to the other 21 items.

2. **No Commit/Branch correlation.** When multiple sessions implement proposals in
   parallel on different local branches, there is no systematic way to find which 
   commit or branch corresponds to which queue item. The `result` field exists but 
   is only set when a session calls `work complete --result "..."`.

3. **No timing data.** There is no record of how long items took to
   complete, which would help estimate queue processing time.

## Goals

- Every state transition is logged with timestamp, session, and reason.
- Completed items retain their branch name, commit SHA, or result summary.
- Queue stats (throughput, avg completion time) are queryable.

## Approach

### 1. Audit log table

Add a `work_queue_log` table:

```sql
CREATE TABLE IF NOT EXISTS work_queue_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id TEXT NOT NULL,
    old_status TEXT,
    new_status TEXT NOT NULL,
    session_id TEXT,
    detail TEXT DEFAULT '',
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_wq_log_item ON work_queue_log(item_id);
```

Every `UPDATE` to `work_queue.status` also inserts a log row.

### 2. Integration into existing commands

- `work_claim()`: log transition `pending -> claimed`
- `work_finish()`: log `claimed -> done` or `claimed -> failed` with result (e.g. branch name or commit hash)
- `work_cancel()`: log `pending -> cancelled`
- `work_release()`: log `claimed -> pending` with reason
- `work_clear()`: log `* -> cleared` for each deleted item

### 3. Stats command

Add `aimee work stats` that queries the log:

```
Queue stats:
  Total items: 36
  Completed: 12 (avg 18m)
  Failed: 2
  Pending: 15
  Claimed: 4
  Cleared: 3
```

### Changes

| File | Change |
|------|--------|
| `src/db.c` | New migration: `work_queue_log` table |
| `src/cmd_work.c` | Add log insert to all state transitions, add `work_stats()` |

## Acceptance Criteria

- [ ] Every state transition creates a log entry
- [ ] `aimee work stats` shows counts and avg completion time
- [ ] Log entries include session ID and detail (result, reason)
- [ ] `aimee work list --status done` shows completed items with results

## Owner and Effort

- **Owner:** TBD
- **Effort:** S-M
- **Dependencies:** work-queue-session-fix (P0 bug must be fixed first)

## Rollout and Rollback

- **Rollout:** DB migration adds log table. Existing items have no history.
- **Rollback:** Revert. Log table remains but unused. No data loss.
- **Blast radius:** None. Additive change.

## Test Plan

- [ ] Unit test: claim creates log entry with correct old/new status
- [ ] Unit test: complete creates log entry with result detail
- [ ] Unit test: stats computes correct counts and timing
- [ ] Manual: process several items, verify audit trail

## Operational Impact

- **Metrics:** Queue throughput visible via `work stats`.
- **Logging:** None (data is in the DB, not stderr).
- **Alerts:** None.
- **Disk/CPU/Memory:** One extra INSERT per state transition. Negligible.

## Priority

P2. Important for debugging and observability but not blocking core workflow.

## Trade-offs

The log table grows unboundedly. Add a prune mechanism (`work clear-log
--older-than 30d`) in a follow-up if it becomes a concern.

