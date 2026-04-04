# Fix: Double git fetch race and git pull in worktree creation

**Owner:** JBailes
**Status:** Pending

## Problem

Two bugs in aimee's startup git operations:

1. **Double git fetch / race condition:** `cmd_session_start()` forks a background child to run `git fetch + git pull` on the current workspace, then `worktree_ensure()` runs a synchronous `git fetch + git pull` on the same repo. The two fetches race for `.git/FETCH_HEAD.lock`, causing intermittent failures. The background fetch is wasted work.

2. **git pull in worktree_ensure corrupts state:** `worktree_ensure()` runs `git pull --quiet origin main` on the workspace root. This modifies the user's main repo working tree (not the worktree), can fail if the repo is not on the main branch, and is unnecessary since `git fetch` alone updates the remote refs needed for `git worktree add`.

## Solution

1. Remove the forked background fetch from `cmd_session_start()`. Keep only the `fetched_mask` marking so `fetch_workspace_if_needed` knows the workspace is handled. The actual fetch happens synchronously in `worktree_ensure()`.

2. Remove the `git pull` call from `worktree_ensure()`, keeping only `git fetch`. This avoids modifying the workspace working tree and eliminates the branch-mismatch failure mode.

## Acceptance Criteria

- [ ] No background fork for git fetch in `cmd_session_start()`
- [ ] `worktree_ensure()` only runs `git fetch`, not `git pull`
- [ ] `fetched_mask` is still set for the current workspace
- [ ] Project compiles cleanly
- [ ] Lint passes

## Test Plan

- Start a new aimee session and verify worktree creation succeeds
- Verify no `.git/FETCH_HEAD.lock` race errors during startup
- Verify the main repo working tree is not modified during session start

## Rollback Plan

Revert the commit; the previous behavior (double fetch + pull) is functional, just racy.

## Operational Impact

- Startup may be marginally slower since the fetch is now synchronous only (no background pre-fetch), but this is offset by removing the duplicate fetch.
- Eliminates intermittent FETCH_HEAD.lock errors.
- Eliminates unexpected working tree modifications in the main repo.
