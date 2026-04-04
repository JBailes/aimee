# Proposal: Delegate Git Write Workflows in Isolated Worktrees

## Problem

The original version of this proposal predates two important changes:

- delegates already have tool-enabled execution via `--tools`
- primary agents already have compact MCP git tools for routine status, diff,
  commit, push, and PR operations

What is still missing is the high-value case that MCP tools do not solve well:
longer git write workflows delegated into isolated branch-specific worktrees,
especially rebases and conflict resolution.

Today a delegate can inspect git state, but there is no repo-owned workflow for:

- assigning a delegate to a specific branch/worktree
- safely allowing git write commands like `rebase`, `add`, `commit`, and
  `push --force-with-lease`
- cleaning up or preserving the temporary worktree afterward

## Goals

- Let delegates execute bounded git write workflows in isolated worktrees
- Reuse the existing delegate tool loop instead of inventing a second execution
  path
- Keep routine git operations on the MCP path and reserve delegation for the
  multi-step cases

## Approach

### 1. Add a worktree-scoped delegate mode

Add `--worktree-branch <name>` (or equivalent) to `aimee delegate`. The command
creates a temporary worktree for the target branch and runs the delegate inside
that worktree.

### 2. Add a git-write tool profile

Do not expose unrestricted shell. Instead, add a delegate tool profile that
permits:

- `git status`
- `git diff`
- `git add`
- `git commit`
- `git rebase`
- `git push --force-with-lease`

and blocks unrelated commands by default.

### 3. Add a first-class `git` role

Provide a built-in role tuned for:

- rebase onto a base ref
- resolve common conflict patterns
- run configured verification
- push safely

This proposal builds on the existing MCP git tools and `git verify` work rather
than replacing them.

## Changes

| File | Change |
|------|--------|
| `src/cmd_agent_trace.c` | Add worktree-scoped delegate execution flags |
| `src/agent_tools.c` | Add a constrained git-write tool profile for delegates |
| `src/guardrails.c` | Enforce the allowed command surface for git-write delegates |

## Acceptance Criteria

- [ ] A delegate can be launched in an isolated worktree for a target branch
- [ ] A git-role delegate can rebase, resolve conflicts, verify, and push in that worktree
- [ ] Parallel delegates on different branches do not interfere with each other
- [ ] Cleanup behavior is explicit: keep or remove the temporary worktree

## Owner and Effort

- **Owner:** aimee core
- **Effort:** M
- **Priority:** P1

## Test Plan

- [ ] Unit test: worktree setup/teardown behavior
- [ ] Unit test: git-write tool profile rejects commands outside the allowlist
- [ ] Integration test: two delegates operate on separate branches concurrently
- [ ] Integration test: rebase conflict flow leaves the repo in an inspectable state on failure
