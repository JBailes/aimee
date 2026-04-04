# Proposal: Intent Classification for Planning Depth

## Problem

All tasks currently go through the same planning pipeline regardless of complexity. A trivial typo fix gets the same heavyweight plan generation as a cross-cutting architectural refactor. This wastes time and tokens on simple tasks and under-plans complex ones where the lightweight path is taken manually.

Evidence: oh-my-openagent's Prometheus agent (`src/agents/prometheus/interview-mode.ts`) classifies requests into intent types (trivial, simple, refactoring, greenfield, architecture, research, collaborative) and adapts its planning depth accordingly. Trivial tasks skip planning entirely; architecture tasks get deep consultation.

## Goals

- Classify incoming requests by complexity before planning
- Skip planning for trivial tasks (single file, obvious fix)
- Use lightweight planning for medium tasks (1-2 files, clear scope)
- Use full planning with gap analysis for complex tasks (3+ files, architectural)
- Classification should be fast (<1s) and based on heuristics, not LLM calls

## Approach

Add an intent classification step to the beginning of `agent_plan.c`. Analyze the request text and any referenced files to estimate complexity. Route to the appropriate planning depth.

### Classification heuristics

| Signal | Weight | Direction |
|--------|--------|-----------|
| Number of files mentioned | High | More files → more complex |
| Keywords: "fix", "typo", "rename" | Medium | → simpler |
| Keywords: "refactor", "redesign", "migrate" | Medium | → complex |
| Keywords: "architecture", "infrastructure" | High | → complex |
| Request length (words) | Low | Longer → more complex |
| Number of components/modules referenced | High | More → complex |

### Complexity tiers

- **Trivial** (score < 2): No plan. Execute directly.
- **Simple** (score 2–4): Lightweight plan. 1-2 clarifying questions, then execute.
- **Complex** (score > 4): Full planning with validation and gap analysis.

### Changes

| File | Change |
|------|--------|
| `src/agent_plan.c` | Add `plan_classify_intent()` at entry; gate planning depth on result |
| `src/headers/agent.h` | Add intent classification enum and scoring struct |

## Acceptance Criteria

- [ ] "Fix typo in login.c" classifies as trivial
- [ ] "Refactor the auth module across 5 files" classifies as complex
- [ ] "Add a new API endpoint" classifies as simple or complex depending on scope words
- [ ] Classification adds <100ms to request handling
- [ ] Classification can be overridden via `--plan-depth` flag

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (1–2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; active by default
- **Rollback:** Remove classification; all tasks use full planning as before
- **Blast radius:** Affects planning pipeline only; execution unchanged

## Test Plan

- [ ] Unit test: trivial keywords score low
- [ ] Unit test: complex keywords score high
- [ ] Unit test: multi-file references increase score
- [ ] Unit test: override flag bypasses classification
- [ ] Integration test: trivial request skips planning, complex request gets full plan

## Operational Impact

- **Metrics:** Distribution of requests by complexity tier
- **Logging:** Log classification result at info level
- **Disk/CPU/Memory:** Negligible — string matching on request text

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Intent Classification | P2 | M | Medium — reduces overhead for simple tasks |

## Trade-offs

Alternative: always plan, but vary plan detail level. Still wastes the planning round-trip for trivial tasks. Skipping entirely for trivial tasks is more efficient.

Alternative: use an LLM call for classification. More accurate but adds latency and cost. Heuristics cover the obvious cases; edge cases can fall back to the medium tier.

Inspiration: oh-my-openagent `src/agents/prometheus/interview-mode.ts`
