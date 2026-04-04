# Review: work-queue-stale-claim-gc

## Reviewer context

I observed 4+ orphaned "claimed" items from sessions that had finished or
crashed. These items blocked the queue. I also implemented a partial fix in
PR #35 that releases stale claims during `prune_stale_sessions()` in
`cmd_hooks.c`, but that only runs at session wrapup and requires sessions
to have state files older than 24 hours.

## Feedback on approach

### Fix 1: Auto-release stale claims during `work claim`

Good approach. The 2-hour timeout is reasonable for aimee work items (most
complete in minutes). However, consider that the auto-release should also
run during `work list` and `work gc`, not just during `claim`. A user
running `work list` and seeing stale claims should see them cleaned up
automatically.

Implementation note: the SQL should use `julianday('now') - julianday(claimed_at) > 2.0/24.0`
instead of `datetime('now', '-2 hours')` because `claimed_at` is stored as
a TEXT timestamp by `now_utc()`, not as a datetime. Check the exact format
that `now_utc()` writes (ISO 8601) to ensure the comparison works.

### Fix 2: Scoped clear

Good addition. One refinement: allow prefix matching on session ID
(`--session 12fe` matches `12fe2032-...`) since session IDs are long and
the user typically only has the first 8 chars from `work list` output.

### Fix 3: `aimee work gc`

Good. Should report both the number of items released and their titles so
the user can verify nothing was incorrectly released.

## Interaction with PR #35

My PR #35 added stale claim release in `prune_stale_sessions()` but with a
24-hour threshold (matching the existing stale session detection). This GC
proposal uses 2 hours, which is more appropriate for the work queue context.
The two mechanisms are complementary: the session pruner catches very old
orphans, and the work queue GC catches recent ones.

## Priority agreement

P1 is correct. The queue degrades without this but is not completely broken
(items can still be manually released).
