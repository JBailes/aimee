# Branch Ownership Enforcement

## Problem

Multiple Claude Code sessions share the same repos. When Session A creates a branch and PR, Session B can push commits to the same branch, overwriting Session A's work (seen in PR #164).

## Solution

Add a `branch_ownership` table to track which session owns each branch. Enforce ownership in the MCP git tools:

- `git_branch` create: registers ownership automatically
- `git_branch` claim: allows taking ownership of an unowned branch
- `git_branch` delete: cleans up ownership record
- `git_commit`, `git_push`, `git_pr` create: blocked if branch is owned by another session
- `main`/`master`: always shared, never owned

## Implementation

- DB migration 39: `branch_ownership` table with `UNIQUE(repo_path, branch_name)`
- Thread-local DB pointer (`mcp_db_set/get/clear`) passed through `dispatch_git_tool`
- Static helpers in `mcp_git.c`: `branch_own_register`, `branch_own_check`, `branch_own_delete`
- Error messages tell the user to `git_branch action=claim` if they need to take over

## Bugs Fixed (2026-04-05)

Five enforcement gaps were found and fixed:

1. **Missing ownership checks in commit/push/PR/reset**: The proposal specified these
   operations should be blocked when another session owns the branch, but the checks
   were never implemented. Added `branch_own_guard()` to all four handlers.

2. **Worktree repo path mismatch**: `get_repo_path()` used `git rev-parse --show-toplevel`
   which returns the worktree root, not the main repo root. This meant ownership records
   from the main checkout didn't match when checked from a worktree, silently bypassing
   all enforcement. Fixed to use `--git-common-dir` to derive the canonical repo root.

3. **Sticky `s_in_worktree` flag**: The worktree flag was set but never reset between
   dispatch calls, and never read by any code. Fixed to reset at the start of each
   `dispatch_git_tool` call.

4. **Worktree branch creation skipped ownership registration**: `worktree_create_sibling`
   created branches via `git worktree add -b` without registering them in the ownership
   table. Added `mcp_git_branch_own_register()` call after successful worktree creation.

5. **Tests validated buggy behavior**: The test suite explicitly confirmed that commit/push
   were allowed despite other-session ownership. Updated tests to assert blocked behavior.

## Status

Implemented and tested.
