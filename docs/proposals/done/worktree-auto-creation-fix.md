# Proposal: Fix Automatic Worktree Creation for New Sessions

## Problem

When a new session starts, aimee should automatically create worktrees for each workspace project. Currently, the worktree creation fails silently when the workspace name in the database doesn't match the actual directory name (e.g., `aimee-dev` vs `aimee`), or when the worktree path doesn't exist yet.

Observed behavior:
- Session reminder references a worktree path that doesn't exist on disk
- `aimee worktree list` shows worktrees pointing to workspace `aimee-dev` which doesn't exist
- New sessions cannot create worktrees because the workspace root resolution fails

## Goals

- Worktrees are automatically created at session start for all configured workspaces
- Workspace name mismatches between config and disk are detected and corrected
- Clear error messages when worktree creation fails

## Approach

1. During `cmd_session_start`, validate that each workspace root exists on disk
2. If a workspace name doesn't resolve, attempt to find the actual directory
3. Add a `--repair` flag to `aimee worktree` to clean up stale references
4. Log a clear warning when workspace paths are misconfigured

### Changes

| File | Change |
|------|--------|
| src/cmd_hooks.c | Validate workspace roots during session-start |
| src/guardrails.c | Better error handling in worktree_ensure() |
| src/workspace.c | Add workspace root validation/repair |

## Acceptance Criteria

- [ ] New sessions create worktrees without manual intervention
- [ ] Mismatched workspace names produce actionable warnings
- [ ] `aimee worktree list` only shows valid, resolvable worktrees

## Owner and Effort

- **Owner:** aimee core
- **Effort:** S
- **Priority:** P1

## Test Plan

- [ ] Session start creates worktrees for all workspaces
- [ ] Invalid workspace paths produce clear error messages
