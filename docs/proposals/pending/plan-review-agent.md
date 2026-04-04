# Proposal: Plan Review Agent (Momus Pattern)

## Problem

Generated plans can reference files that have been renamed, functions that don't exist, or line numbers that have drifted. A delegate working from a plan with invalid references wastes significant time searching for things that aren't there, or worse, modifies the wrong code. There is no validation step between plan generation and execution.

Evidence: oh-my-openagent implements a "Momus" review agent (`src/agents/momus.ts`) that reviews plans with a practical focus: verifying file references exist, line numbers point to relevant code, and tasks have enough context to start. It has an explicit approval bias — approve unless there's a blocking issue.

## Goals

- Validate all file references in a generated plan actually exist
- Verify referenced symbols/functions exist via the index
- Flag tasks with zero starting context (no file, no pattern, no description)
- Auto-approve plans that are 80%+ valid; only reject on blocking issues
- Run automatically between plan generation and execution

## Approach

After `agent_plan.c` generates a plan, run a validation pass. Parse the plan markdown for file paths, function names, and line references. Check each against the filesystem and symbol index. Produce a validation report: pass (proceed to execution), or fail (list blocking issues for the planner to fix).

### Validation checks

1. **File existence**: every `src/foo.c` reference → `access(path, F_OK)`
2. **Symbol existence**: every function/struct name → `index_find(symbol)`
3. **Task executability**: every task has at least one of: file path, symbol reference, or >20 words of description
4. **No contradictions**: tasks don't reference the same file with conflicting instructions

### Changes

| File | Change |
|------|--------|
| `src/agent_plan.c` | Add `plan_validate()` post-generation step |
| `src/index.c` | Expose `index_find_symbol()` for plan validation |
| `src/headers/agent.h` | Add plan validation result struct |

## Acceptance Criteria

- [ ] Plans with invalid file references are flagged
- [ ] Plans with valid references pass without intervention
- [ ] Missing symbols are reported with suggestions (did-you-mean via index)
- [ ] Validation adds <2s to plan generation
- [ ] Plans with >80% valid references auto-approve with warnings for the remainder

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2–3 days)
- **Dependencies:** Symbol index must be populated for the project

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; runs automatically on plan generation
- **Rollback:** Skip validation step; plans proceed unvalidated as before
- **Blast radius:** Only affects plan generation pipeline; execution unaffected

## Test Plan

- [ ] Unit test: plan with valid references passes
- [ ] Unit test: plan with nonexistent file is flagged
- [ ] Unit test: plan with renamed function is flagged with suggestion
- [ ] Unit test: plan with vague task (no references, <20 words) is flagged
- [ ] Integration test: end-to-end plan generation with validation

## Operational Impact

- **Metrics:** Plan validation pass/fail rate, count of invalid references per plan
- **Logging:** Log validation results at info level
- **Disk/CPU/Memory:** Index lookups and file stat calls; negligible for typical plans (<50 references)

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Plan Review Agent | P1 | M | High — prevents delegates from working with invalid plans |

## Trade-offs

Alternative: validate at execution time (when the delegate first tries to access a file). Too late — the delegate has already committed context to the plan's assumptions. Pre-validation catches issues before any tokens are spent on execution.

Alternative: full plan review by a separate LLM call. Expensive and slow. Mechanical validation (file exists, symbol exists) covers the most impactful checks without LLM cost.

Inspiration: oh-my-openagent `src/agents/momus.ts`
