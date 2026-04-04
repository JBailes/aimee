# Proposal: Automatic Outcome Signals from Agent Execution

## Problem

Agents complete tasks via `agent_run()` but never self-report structured outcomes (success/partial/failure + reason). The eval-feedback loop (`agent_eval.c`, `eval_feedback_loop` in `cmd_hooks.c`) only learns from curated test suites, not real production work. This means the system cannot learn from the vast majority of agent executions. The feedback system (`feedback.c`) requires manual human input via `aimee feedback record`.

## Goals

- `agent_run()` emits a structured outcome signal after every execution
- Outcome signals feed into the eval-feedback loop automatically
- The system learns from real work, not just test suites
- No manual intervention required for the learning loop

## Approach

### 1. Outcome Struct

Define an outcome struct in `agent.h`:

```c
typedef enum {
    OUTCOME_SUCCESS,
    OUTCOME_PARTIAL,
    OUTCOME_FAILURE,
    OUTCOME_ERROR
} outcome_type_t;

typedef struct {
    bool success;
    outcome_type_t outcome;
    char *reason;
    int turns_used;
    int tools_called;
    int64_t tokens_used;
} agent_outcome_t;
```

### 2. Outcome Classification

After `agent_run()` completes in `agent.c`, classify the outcome by inspecting:

- Whether the agent produced a final answer
- Whether any tool calls errored
- Whether `max_turns` was hit (likely failure)
- Whether a verify command passed

### 3. Persistence

Record the outcome to an `agent_outcomes` table in the database (similar to `eval_results` but for real work).

### 4. Feedback Loop Integration

In `eval_feedback_loop()` (`cmd_hooks.c`), expand the query to include `agent_outcomes` alongside `eval_results` when adjusting rule weights.

### 5. Anti-pattern Extraction

If the same tool+error pattern appears in 3+ outcomes, auto-create an anti-pattern via `memory_advanced.c`.

### Changes

| File | Change |
|------|--------|
| `src/headers/agent.h` | Add `agent_outcome_t` struct and `outcome_type_t` enum |
| `src/agent.c` | Emit outcome after `agent_run()` completes |
| `src/db.c` | Add `agent_outcomes` table migration |
| `src/cmd_hooks.c` | Expand `eval_feedback_loop` to include outcomes |
| `src/memory_advanced.c` | Add outcome-based anti-pattern extraction |

## Acceptance Criteria

- [ ] `agent_run()` records an outcome for every execution (verify via `SELECT count(*) FROM agent_outcomes`)
- [ ] Outcome classification correctly identifies: max_turns hit as failure, verify pass as success, tool errors as partial/error
- [ ] `eval_feedback_loop()` queries `agent_outcomes` alongside `eval_results` when adjusting rule weights
- [ ] Anti-pattern auto-created when the same tool+error pattern appears in 3+ outcomes
- [ ] Existing agent behavior is unchanged (outcome recording is passive)

## Owner and Effort

- **Owner:** aimee
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** New migration adds `agent_outcomes` table. Outcome recording activates immediately for all agent executions. No feature flag needed since this is passive instrumentation.
- **Rollback:** Revert commit. Outcome data is append-only and safe to delete. No existing behavior changes.
- **Blast radius:** Low. Outcome recording is passive (does not change agent behavior). Rule weight adjustments from outcomes are bounded by the same +10/-5 caps as eval results.

## Test Plan

- [ ] Unit tests: outcome classification heuristics (max_turns hit, verify pass/fail, tool errors, clean completion)
- [ ] Integration tests: run agent, verify outcome recorded in `agent_outcomes` table
- [ ] Integration tests: verify `eval_feedback_loop` processes outcomes alongside eval results
- [ ] Failure injection: database write failure during outcome recording does not crash the agent
- [ ] Manual verification: run `aimee delegate`, inspect outcome row, confirm classification matches observed behavior

## Operational Impact

- **Metrics:** New `agent_outcomes` count by outcome type.
- **Logging:** New log line on outcome recording at DEBUG level.
- **Alerts:** None.
- **Disk/CPU/Memory:** `agent_outcomes` table grows ~1 row per agent execution. Negligible disk and CPU impact.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Outcome struct and classification | P1 | S | Enables all downstream learning |
| Database persistence | P1 | S | Required for feedback loop integration |
| Feedback loop integration | P1 | S | Core value: system learns from real work |
| Anti-pattern extraction | P1 | S | Prevents repeated mistakes automatically |

## Trade-offs

**Why not require explicit success criteria for every delegation?** Most delegations are ad-hoc and requiring criteria adds friction. Heuristic classification (max_turns hit = failure, verify command passed = success) covers ~80% of cases. Explicit criteria can be added optionally via the `--verify` flag.

**Why not store outcomes in a separate file instead of the database?** The eval-feedback loop already queries the database for `eval_results`. Storing outcomes in the same database allows a single query to combine both sources, avoiding file parsing overhead and consistency issues.

**Why bounded rule weight adjustments?** Unbounded adjustments from noisy real-world data could destabilize the rule system. The existing +10/-5 caps from eval results apply equally to outcome-derived adjustments, providing a natural safety bound.
