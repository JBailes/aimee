# Proposal: Hierarchical Instruction File Discovery with Budget Caps

## Problem

Aimee's rules system (`rules.c`) loads project-level rules but lacks:

1. **Ancestor-chain discovery** — a monorepo with rules at multiple directory levels should load all of them when CWD is deep in the tree.
2. **Content deduplication** — duplicated/symlinked rule files are loaded multiple times, wasting context.
3. **Budget caps** — a verbose rules file can consume disproportionate system prompt space.

These issues affect all sessions equally — CLI and webchat both load rules into context.

The `soongenwong/claudecode` repo at `rust/crates/runtime/src/prompt.rs` implements ancestor-chain discovery, content-hash deduplication, and per-file/total budget caps.

## Goals

- Rules files are discovered from CWD through every ancestor to the project root.
- Duplicate content is detected by hash and skipped.
- Per-file (4K) and total (12K) budget caps prevent context bloat.
- Works identically in CLI and webchat sessions.

## Approach

### Discovery Walk

```
CWD: /root/dev/aimee/src/tests/
Search:
  1. /root/dev/aimee/src/tests/.aimee-rules
  2. /root/dev/aimee/src/.aimee-rules
  3. /root/dev/aimee/.aimee-rules
  4. ~/.config/aimee/global-rules  (always loaded)
```

Files closer to CWD get priority (loaded first, more likely to fit within budget).

### Changes

| File | Change |
|------|--------|
| `src/rules.c` | Add `rules_discover_ancestor_chain()`: walk CWD to root, deduplicate by content hash, apply budget caps |
| `src/agent_context.c` | Use discovered rules chain instead of single project-root rules |
| `src/config.c` | Add `rules_per_file_budget`, `rules_total_budget` config |
| `src/webchat.c` | Use same discovery logic for webchat session context |

## Acceptance Criteria

- [ ] Rules in nested directories are loaded when CWD is deeper
- [ ] Duplicate files (same content) are loaded only once
- [ ] Budget caps truncate oversized files and stop loading when total is exceeded
- [ ] CLI and webchat use the same discovery logic

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S (1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Behavior change: ancestor rules are now loaded. Budget caps are configurable.
- **Rollback:** Set discovery depth to 0 (project root only).
- **Blast radius:** Unexpected ancestor rules could inject unwanted instructions. Log discovered files at INFO.

## Test Plan

- [ ] Unit tests: ancestor walk, deduplication, budget truncation
- [ ] Integration tests: nested .aimee-rules files, verify all loaded in correct order
- [ ] Manual verification: place rules at multiple levels, confirm all appear in context

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Ancestor-chain discovery | P2 | S | High — monorepo support |
| Budget caps | P2 | S | High — prevents context bloat |
| Content deduplication | P3 | S | Low — edge case |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/runtime/src/prompt.rs`.
