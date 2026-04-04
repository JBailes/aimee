# Proposal: Active AI Slop Removal

## Problem

Proposal #4 (comment-slop-checker) detects AI slop patterns but only warns. Delegates often ignore warnings and the slop remains in the codebase. A more aggressive approach would actively remove known slop patterns from code, not just flag them. This includes: redundant comments, over-defensive null checks, spaghetti nesting, backward-compat shims, and unused re-exports.

Evidence: oh-my-openagent implements a full `ai-slop-remover` skill (`src/features/builtin-skills/skills/ai-slop-remover.ts`) that operates on individual files, actively refactoring to remove AI-generated patterns while preserving functionality. It has detailed detection criteria with explicit keep/remove rules for each pattern category.

## Goals

- Actively remove AI slop from code files, not just detect it
- Preserve functionality — no behavioral changes
- Clear keep/remove criteria for each pattern category
- Operate on one file at a time for safety
- Usable as a post-delegate cleanup step

## Approach

Add an `aimee clean <file>` command that delegates to a code-cleaning agent. The agent reads the file, identifies slop patterns, and produces an edited version with patterns removed.

### Pattern categories with keep/remove rules

**Comments:**
- REMOVE: restating code, trivial docstrings, section dividers, commented-out code, vague TODOs
- KEEP: business logic explanations, issue links, algorithm explanations, regex explanations

**Defensive code:**
- REMOVE: null checks for non-nullable values, try-except around non-throwing code, isinstance for typed params
- KEEP: validation at system boundaries, I/O error handling, nullable DB field checks

**Structure:**
- REFACTOR: nested if-else → early returns, complex ternaries → explicit if-else
- KEEP: necessary nesting for complex business logic

### Changes

| File | Change |
|------|--------|
| `src/cmd_util.c` | Add `aimee clean <file>` command |
| `src/agent_tools.c` | Implement clean-file delegate with slop removal prompt |

## Acceptance Criteria

- [ ] `aimee clean src/foo.c` produces a cleaned version
- [ ] Redundant comments are removed
- [ ] Over-defensive checks are removed
- [ ] Legitimate comments and checks are preserved
- [ ] No behavioral changes in the cleaned code
- [ ] Diff is reviewable before committing

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2–3 days)
- **Dependencies:** Delegation system functional

## Rollout and Rollback

- **Rollout:** New CLI command; no changes to existing behavior
- **Rollback:** Remove command; code unchanged
- **Blast radius:** Only affects files explicitly targeted by the command

## Test Plan

- [ ] Unit test: redundant comment is removed
- [ ] Unit test: business logic comment is preserved
- [ ] Unit test: unnecessary null check is removed
- [ ] Unit test: boundary validation is preserved
- [ ] Integration test: clean a file with known slop, verify output

## Operational Impact

- **Metrics:** Files cleaned, patterns removed per file
- **Logging:** Log at info level with pattern counts
- **Disk/CPU/Memory:** One delegate session per file; proportional to file size

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Active Slop Removal | P2 | M | Medium — actively improves code quality |

## Trade-offs

Alternative: integrate into the edit pipeline (clean as you write). Too aggressive — may remove intentional patterns during active development. Explicit invocation is safer.

Alternative: run as a pre-commit hook. Good complement but doesn't catch slop during development. Both approaches are valuable.

Inspiration: oh-my-openagent `src/features/builtin-skills/skills/ai-slop-remover.ts`
