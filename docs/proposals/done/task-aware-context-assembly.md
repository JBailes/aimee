# Proposal: Task-Aware Context Assembly

## Problem

agent_build_exec_context() in src/agent_context.c:468 assembles context for delegates by keyword-matching the prompt against memory and code symbols. This is task-type-agnostic: a refactoring task gets the same context shape as a bug fix or a new feature. The result is that delegates often get irrelevant context (architecture docs for a typo fix) or miss critical context (recent error patterns for a bug investigation).

The context budget is fixed at AGENT_CONTEXT_BUDGET = 16,000 chars (agent_types.h:30). Wasting budget on irrelevant context directly reduces the quality of delegate work.

## Goals

- Context assembly adapts to task type (bug fix, refactor, feature, review, test).
- Higher-signal context items are prioritized within the fixed budget.
- Delegate success rate improves for specialized tasks.

## Approach

### 1. Task type classification

Classify the delegation prompt into a task type using keyword heuristics:

| Type | Signal words |
|------|-------------|
| bug_fix | fix, bug, error, crash, fail, broken, regression |
| refactor | refactor, rename, extract, move, reorganize, clean |
| feature | add, implement, create, new, build, support |
| review | review, check, audit, verify, validate |
| test | test, coverage, assert, expect, spec |

Use the first matching type. Default to 'general' if no match.

### 2. Type-specific context weights

Each task type gets a priority weighting for context categories:

| Category | bug_fix | refactor | feature | review | test |
|----------|---------|----------|---------|--------|------|
| Recent errors/episodes | HIGH | LOW | LOW | MED | MED |
| Architecture/structure | LOW | HIGH | HIGH | HIGH | LOW |
| Related code symbols | HIGH | HIGH | MED | HIGH | HIGH |
| Procedures/how-to | MED | LOW | MED | LOW | HIGH |
| Recent changes (git) | HIGH | MED | LOW | HIGH | MED |

### 3. Budget allocation

Split the 16K budget proportionally:
- HIGH categories get 40% of remaining budget each (capped)
- MED categories get 25%
- LOW categories get 10%

Fill categories in priority order until budget exhausted.

## Changes

| File | Change |
|------|--------|
| src/agent_context.c | Add task_type_classify() heuristic, modify agent_build_exec_context() to use type-specific weights |
| src/headers/agent_types.h | Add task_type_t enum |

## Acceptance Criteria

- [ ] Task type is classified from delegation prompt
- [ ] Context assembly uses type-specific priority weights
- [ ] Bug fix tasks prioritize recent errors and related code over architecture
- [ ] Refactor tasks prioritize architecture and code structure over errors
- [ ] Context budget is not exceeded regardless of type
- [ ] Unclassified tasks fall back to current balanced behavior

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change. All delegations benefit automatically.
- **Rollback:** git revert. Falls back to current unweighted assembly.
- **Blast radius:** All delegations. Misclassification could provide suboptimal context, but never worse than random.

## Test Plan

- [ ] Unit test: task_type_classify() maps known prompts to expected types
- [ ] Unit test: budget allocation respects weights and caps
- [ ] Integration test: delegate with 'fix the crash in X' gets error-heavy context
- [ ] Integration test: delegate with 'refactor Y into Z' gets architecture-heavy context
- [ ] Manual verification: compare context quality before/after on real tasks

## Operational Impact

- **Metrics:** None.
- **Logging:** Log classified task type at debug level.
- **Alerts:** None.
- **Disk/CPU/Memory:** Negligible. Classification is O(n) string matching on prompt.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Task-aware context | P1 | M | Directly improves delegate effectiveness |

## Trade-offs

Keyword heuristics are imperfect. A prompt like 'add error handling' could be classified as feature or bug_fix. The fallback to 'general' prevents catastrophic misclassification. A more sophisticated classifier (e.g., embedding similarity) adds complexity for marginal gain at this scale.
