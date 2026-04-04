# Review: work-queue-session-fix

## Reviewer context

I hit this exact bug in my session. After claiming 6 items and completing
each with a PR, `aimee work complete` failed every time with "No matching
claimed work item found." I also could not complete via `--id` due to the
`claimed_by` check. The queue showed 0 done / 0 failed items despite 6 PRs
being created.

## Feedback on approach

### Fix 1: Remove session check from `--id` path

Agree completely. When a user passes `--id`, they are explicitly targeting
an item. The session check adds no security value (single-user tool) and
breaks the primary use case. This is the right fix.

### Fix 2: Persist session ID to worktree directory

This is correct but needs a detail: the `.aimee-session-id` file should be
written at worktree creation time (in `session-start`), not lazily in
`session_id()`. If `session_id()` writes the file, there is a race between
the first CLI call that creates the file and subsequent calls that read it.
Writing it during `session-start` (which already creates the worktree)
avoids the race.

Also consider: the file should be in `.aimee-session-id` at the worktree
root, not in `~/.config/aimee/`, so multiple worktrees each have their own
stable ID.

## Additional suggestion

The `CLAUDE_SESSION_ID` env var fallback already works when set. The
worktree persistence is the right approach for when it is not set. But also
consider: `aimee work claim` could echo the session ID so the user can
verify they are in the right session context.

## Priority agreement

P0 is correct. The work queue is non-functional without this fix.
