# Proposal: Cache Base Branch Per Workspace

## Problem

`worktree_ensure()` in `src/guardrails.c:926-935` tries three base branches sequentially when creating a worktree: `main`, `origin/main`, `HEAD`. Each attempt spawns a `git worktree add` subprocess. If the first doesn't exist (e.g., the repo uses `master`), we pay ~50-100ms per failed fork+exec before finding the right one.

## Goals

- First worktree creation attempt succeeds in the common case.
- No wasted subprocess spawns for branches that don't exist.

## Approach

Store the last-successful base branch name in the `worktree_entry_t` struct. On first use, populate it by checking which ref exists (using `git rev-parse --verify` which is fast and local). On subsequent worktree creations for the same workspace, use the cached value directly.

For the initial population: run `git rev-parse --verify main` first. If it succeeds, use `main`. Otherwise try `origin/main`, then `HEAD`. This is a single fast subprocess per unknown workspace rather than up to 3 `git worktree add` attempts.

### Changes

| File | Change |
|------|--------|
| `src/headers/guardrails.h` | Add `char base_branch[64]` to `worktree_entry_t` |
| `src/guardrails.c` | In `worktree_ensure()`: detect the correct base branch via `git rev-parse --verify` before attempting `git worktree add`; store result in `entry->base_branch`; use it directly for the worktree add call |
| `src/guardrails.c` | Serialize/deserialize `base_branch` in session state load/save |

## Acceptance Criteria

- [ ] `worktree_ensure()` spawns at most 1 `git worktree add` process (not up to 3)
- [ ] Base branch detection uses `git rev-parse --verify` (fast, local, no network)
- [ ] `base_branch` field persists in session state JSON
- [ ] All existing tests pass

## Owner and Effort

- **Owner:** JBailes
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change.
- **Rollback:** `git revert`. Old session state files without `base_branch` will just use the detection logic (graceful fallback).
- **Blast radius:** Worktree creation only.

## Test Plan

- [ ] Unit tests: base branch detection picks `main` when it exists
- [ ] Unit tests: base branch detection falls back to `origin/main` then `HEAD`
- [ ] Integration tests: worktree creation works with cached base branch
- [ ] Manual verification: strace shows single `git worktree add` call

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** Saves 1-2 subprocess spawns (~50-100ms each).

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Cache base branch | P2 | S | Saves 50-200ms on worktree creation |

## Trade-offs

Adds 64 bytes to `worktree_entry_t` and a field to the session state JSON. Negligible overhead. If the repo's default branch changes between sessions, the detection logic runs again (the cached value is per-session, not persisted across sessions).
