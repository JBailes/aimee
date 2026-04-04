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

## Status

Implemented and tested.
