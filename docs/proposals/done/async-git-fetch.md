# Proposal: Async Git Fetch in Worktree Creation

## Problem

`worktree_ensure()` in `src/guardrails.c:880-884` runs a synchronous `git fetch --quiet origin` before creating the worktree. This blocks the entire startup path for 1-5 seconds depending on network latency to the remote. The fetch was moved here from a background process (PR #141) to fix a race on `FETCH_HEAD.lock`, but the cure placed it directly on the critical path.

## Goals

- Remove `git fetch` from the blocking startup path.
- Worktree creation completes in <100ms (local git operations only).
- Refs are still fetched — just not synchronously before the user can interact.

## Approach

Fork the `git fetch` into a background child process and create the worktree immediately from the current (possibly stale) local refs. The worktree will be based on whatever `main`/`origin/main`/`HEAD` exists locally. The background fetch completes asynchronously; the next session or worktree creation will benefit from the updated refs.

### Changes

| File | Change |
|------|--------|
| `src/guardrails.c` | In `worktree_ensure()`: fork the `git fetch` into a background child (double-fork to avoid zombies), then proceed to create the worktree immediately without waiting |

## Acceptance Criteria

- [ ] `worktree_ensure()` does not block on network I/O
- [ ] `git fetch` still runs (visible via `ps` or strace) but in a detached child
- [ ] Worktree is created successfully from local refs
- [ ] No zombie processes left behind (double-fork or SIGCHLD ignore)
- [ ] All existing tests pass

## Owner and Effort

- **Owner:** JBailes
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change, no feature flags.
- **Rollback:** `git revert` of the commit.
- **Blast radius:** All sessions — worktrees may be based on slightly stale refs until the background fetch completes.

## Test Plan

- [ ] Unit tests: `worktree_ensure()` returns in <200ms (no network wait)
- [ ] Manual verification: `ps aux | grep fetch` shows background fetch running after startup
- [ ] Integration tests: worktree is usable immediately after creation
- [ ] Failure injection: remote unreachable — startup still completes quickly, background fetch fails silently

## Operational Impact

- **Metrics:** None.
- **Logging:** Background fetch child inherits stderr; fetch errors visible in logs.
- **Alerts:** None.
- **Disk/CPU/Memory:** No change — same fetch runs, just non-blocking.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Async git fetch | P0 | S | Saves 1-5s on every worktree creation |

## Trade-offs

The worktree will be based on whatever refs were last fetched locally. In practice this means it's at most one session behind — acceptable since the agent is about to make its own commits. The alternative (prefetching on a timer) adds complexity without clear benefit.
