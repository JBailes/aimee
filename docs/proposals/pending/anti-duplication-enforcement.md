# Proposal: Anti-Duplication Enforcement for Delegates

## Problem

When the orchestrator delegates a research task (e.g., "find all callers of function X"), it sometimes then performs the same search itself — grepping for the same symbols, reading the same files. This wastes expensive orchestrator tokens duplicating work that a cheaper delegate is already doing. It also risks contradictory conclusions if the orchestrator's quick search finds different results than the delegate's thorough one.

Evidence: oh-my-openagent implements explicit anti-duplication rules (`src/agents/dynamic-agent-policy-sections.ts`, `buildAntiDuplicationSection()`). The orchestrator prompt forbids re-searching delegated topics and requires waiting for delegate results before proceeding with dependent work.

## Goals

- Track what queries/topics have been delegated to sub-agents
- Warn the orchestrator when it attempts to search for something already delegated
- Allow non-overlapping work to proceed in parallel
- Reduce wasted tokens on duplicate research

## Approach

Maintain a per-session delegation log that records the topic/query of each delegation. When the orchestrator invokes search tools (grep, glob, index find), compare the query against recent delegations using simple keyword overlap. If overlap exceeds a threshold, inject a warning: "This search overlaps with a pending delegation — wait for delegate results instead."

### Changes

| File | Change |
|------|--------|
| `src/agent_coord.c` | Record delegation topics when dispatching delegates |
| `src/guardrails.c` | Add `guardrails_check_duplication()` comparing tool queries against delegation log |
| `src/headers/guardrails.h` | Delegation log structure |

## Acceptance Criteria

- [ ] Delegated queries are recorded in the session's delegation log
- [ ] Search tool calls with >60% keyword overlap with a pending delegation trigger a warning
- [ ] Warning is advisory (appended to output), not blocking
- [ ] Non-overlapping searches proceed without warning
- [ ] Delegation log entries expire when the delegate completes

## Owner and Effort

- **Owner:** aimee
- **Effort:** S–M (1–2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; active for orchestrator sessions with pending delegations
- **Rollback:** Remove the check; orchestrator searches proceed without overlap detection
- **Blast radius:** Only affects orchestrator search tool output; delegates unaffected

## Test Plan

- [ ] Unit test: delegation is recorded in log
- [ ] Unit test: overlapping search triggers warning
- [ ] Unit test: non-overlapping search does not trigger
- [ ] Unit test: completed delegation is removed from log
- [ ] Integration test: orchestrator delegates grep, then greps for same thing, verify warning

## Operational Impact

- **Metrics:** Count of duplication warnings per session
- **Logging:** Log at info level when overlap detected
- **Disk/CPU/Memory:** One keyword set per pending delegation; negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Anti-Duplication Enforcement | P1 | S–M | Medium — reduces wasted orchestrator tokens |

## Trade-offs

Alternative: block the duplicate search entirely. Too aggressive — there are legitimate reasons to do a quick local check while a thorough delegate search is in flight. Warning-only preserves flexibility.

Alternative: semantic similarity instead of keyword overlap. More accurate but requires embedding computation, which is overkill for this use case. Keyword overlap catches the obvious cases.

Inspiration: oh-my-openagent `src/agents/dynamic-agent-policy-sections.ts`
