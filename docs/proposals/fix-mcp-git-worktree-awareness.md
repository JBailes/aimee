# Proposal: Fix MCP git tools worktree awareness

## Problem

MCP git tool handlers (`handle_git_branch`, `handle_git_push`, `handle_git_pr`) are unaware they are operating inside a session worktree. This causes three bugs:

1. **`git_branch create` changes the worktree's checked-out branch.** It uses `git checkout -b` which both creates AND switches to the new branch, detaching the worktree from its session branch (`aimee/session/<id>`). Worktrees are session-scoped and must remain on their session branch.

2. **`git_push` pushes the wrong ref.** It reads `HEAD` to determine the branch to push. In a worktree, HEAD is the session branch, not the user's named branch. This causes the push to either push the session branch or report a stale ref.

3. **`git_pr create` fails with "No commits between main and <branch>".** `gh pr create` infers the branch from HEAD, which in a worktree is the session branch. Since the session branch may not be ahead of main, GitHub reports no commits. The fix is to pass `--head <branch>` explicitly.

All three bugs share the same root cause: the dispatch in `server_mcp.c` redirects to the worktree directory but doesn't tell the handlers they're in a worktree.

## Goals

- MCP git handlers know when they're operating in a worktree and adjust behavior accordingly.
- `git_branch create` creates branches without switching to them in worktrees.
- `git_push` pushes the session's owned branch (from `branch_ownership`) instead of HEAD.
- `git_pr create` uses `--head <branch>` to specify the correct branch.
- `git_branch switch` is blocked in worktrees with a clear error message.

## Approach

### Worktree context flag

Add a thread-local flag `s_in_worktree` in `mcp_git.c` with accessor functions `mcp_git_set_worktree()` / `mcp_git_in_worktree()`. The dispatch in `server_mcp.c` sets this flag when it detects worktree redirect, and clears it after the handler returns.

### Handler changes

**`handle_git_branch` (create):**
- In worktree: `git branch <name> [base]` (no checkout)
- Outside worktree: `git checkout -b <name> [base]` (unchanged)

**`handle_git_branch` (switch):**
- In worktree: return error explaining worktrees are session-scoped

**`handle_git_push`:**
- In worktree: look up the session's owned branch from `branch_ownership` table, push that branch by explicit refspec
- Outside worktree: unchanged (push HEAD)

**`handle_git_pr` (create):**
- In worktree: look up the session's owned branch, pass `--head <branch>` to `gh pr create`
- Outside worktree: unchanged

### Changes

| File | Change |
|------|--------|
| `src/mcp_git.c` | Add `mcp_git_set_worktree()`/`mcp_git_in_worktree()`, fix `handle_git_branch`, `handle_git_push`, `handle_git_pr` |
| `src/server_mcp.c` | Set worktree flag when redirecting to worktree, clear after dispatch |
| `src/headers/mcp_git.h` | Declare worktree context functions |

## Acceptance Criteria

- [ ] `git_branch create` in a worktree creates the branch without switching to it
- [ ] `git_branch switch` in a worktree returns a clear error
- [ ] `git_push` in a worktree pushes the session's owned branch, not the session branch
- [ ] `git_pr create` in a worktree uses `--head` to specify the correct branch
- [ ] All existing unit tests pass
- [ ] Build succeeds with `-Werror`

## Owner and Effort

- **Owner:** aimee contributor
- **Effort:** S (already implemented)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change, no migration.
- **Rollback:** Revert commit.
- **Blast radius:** Only affects MCP git operations when a session worktree is active.

## Test Plan

- [ ] Existing `unit-test-mcp-git` passes
- [ ] Existing `unit-test-cmd-core` passes
- [ ] Manual test: create branch via MCP in worktree, verify session branch unchanged
- [ ] Manual test: push via MCP in worktree, verify correct branch pushed
- [ ] Manual test: create PR via MCP in worktree, verify correct `--head` used

## Operational Impact

- **Logging:** Worktree-mode messages on stderr unchanged.
- **Disk/CPU/Memory:** Negligible (one extra DB query for branch lookup in worktree mode).
