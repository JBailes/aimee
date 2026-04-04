# Proposal: Cascading Branch Merge Orchestration

## Problem

When merging multiple local feature branches that touch overlapping files (especially db.c migrations and Makefile), each merge changes main and creates new conflicts in the remaining branches. During a session merging 20 feature branches, this required:

1. Merge a batch of clean branches
2. Discover that the remaining branches now conflict with the updated main
3. Rebase each remaining branch again
4. Merge the next batch
5. Repeat until all are merged

This cycle repeated 4 times. The primary conflict pattern was db.c migration numbers: every branch added migration 35, but only one can be 35 after the first merge. Each subsequent branch needs renumbering (36, 37, 38, ...).

Because aimee is a single-user, single-machine, multi-session system, work is primarily done on local branches across isolated worktrees. We need a repo-owned orchestration layer for cross-branch conflict analysis and ordered merge execution, without relying on external PR mechanisms.

## Goals

- Detect when multiple local branches conflict with each other (not just with main)
- Automatically determine a safe merge order that minimizes rebase cycles
- Provide a "merge all" command that handles the cascade automatically
- Auto-resolve common conflict patterns (migration renumbering, additive list merges)

## Approach

### Phase 1: Conflict analysis

Add `aimee branch conflicts` command that:
- Takes a list of branch names (or "all worktree branches")
- For each pair, checks if they touch the same files
- Builds a conflict graph showing which branches conflict with which
- Suggests a merge order (least-conflicting first, dependency-aware)

### Phase 2: Auto-resolution patterns

Add pluggable conflict resolution patterns to `aimee branch merge-all`:
- **Migration renumber:** When two branches add the same migration number, renumber the later one to max+1
- **Additive list merge:** When two branches add items to the same list (e.g., .PHONY, TEST_TARGETS, LINT_SRCS), combine both additions
- **Keep-both blocks:** When two branches add independent code blocks at the same insertion point, keep both

These patterns cover the three most common conflicts seen in this codebase.

### Phase 3: Orchestrated merge

Add `aimee branch merge-all` command that:
1. Runs conflict analysis (Phase 1)
2. Determines merge order
3. For each branch: rebase on latest main, apply auto-resolution patterns, run
   local verification (`aimee git verify`), and merge
4. Reports progress and stops on unresolvable conflicts

### Changes

| File | Change |
|------|--------|
| src/cmd_branch.c (or new cmd_merge.c) | Add `branch conflicts` and `branch merge-all` commands |
| src/headers/aimee.h | Add conflict analysis and auto-resolution types |

## Acceptance Criteria

- [ ] `aimee branch conflicts feature-1 feature-2 feature-3` shows which branches conflict with each other
- [ ] `aimee branch merge-all` merges all targeted branches in dependency-safe order
- [ ] Migration number conflicts auto-resolved by renumbering
- [ ] Additive list conflicts auto-resolved by combining
- [ ] Unresolvable conflicts reported clearly with affected files
- [ ] Process stops cleanly on local verify failure, leaving remaining branches untouched

## Owner and Effort

- **Owner:** aimee core
- **Effort:** M
- **Dependencies:** Builds on the existing MCP git layer rather than raw git
  bash flows
- **Priority:** P2

## Rollout and Rollback

- **Rollout:** Phase 1 (analysis) first, usable standalone. Phase 2 and 3 build on it.
- **Rollback:** Remove the commands. No persistent state.

## Test Plan

- [ ] Unit test: conflict graph construction for known overlapping file sets
- [ ] Unit test: migration renumber auto-resolution produces valid migrations
- [ ] Unit test: additive list merge produces correct combined lists
- [ ] Integration test: `merge-all` on a set of 3 conflicting branches merges them in order

## Operational Impact

- The conflict analysis is read-only and safe to run at any time.
