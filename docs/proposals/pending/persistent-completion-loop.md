# Proposal: Persistent Completion Loop (`aimee complete`)

## Problem

Aimee delegates are fire-and-forget: the primary agent sends a task to a delegate, gets a result, and moves on. There is no built-in mechanism for iterative completion — retrying failed steps, verifying the result, and looping until acceptance criteria are actually met.

When a complex task requires multiple delegation rounds (implement → test → fix → retest), the primary agent must manually orchestrate this loop, spending expensive tokens on coordination logic that should be infrastructure.

oh-my-codex's `$ralph` skill solves this with a persistent completion loop: iterate delegation + verification + architect sign-off until the task is verifiably done, with state persisted across iterations. The key insight is that "keep going until done" should be a first-class execution mode, not ad-hoc.

Evidence:
- `agent_plan.c` can track step status (pending/running/done/failed) but has no retry or re-verification loop
- `git_verify.c` runs verification steps but only as a pre-push gate, not as part of an iterative completion cycle
- Delegate attempts are recorded (`record_attempt`) but failures don't trigger automatic retries or alternative strategies

## Goals

- A plan can be executed in "completion mode" where failed steps are retried and the overall result is verified before declaring success
- Iteration state is persisted in the DB, surviving session boundaries
- A configurable verification command runs after each iteration to check progress
- The loop caps at a configurable maximum iteration count to prevent runaway spend
- The same error appearing 3+ times triggers escalation rather than infinite retry

## Approach

### 1. Add completion loop state to execution plans

```sql
ALTER TABLE execution_plans ADD COLUMN completion_mode INTEGER DEFAULT 0;
ALTER TABLE execution_plans ADD COLUMN completion_iteration INTEGER DEFAULT 0;
ALTER TABLE execution_plans ADD COLUMN completion_max_iterations INTEGER DEFAULT 5;
ALTER TABLE execution_plans ADD COLUMN completion_verify_cmd TEXT;
ALTER TABLE execution_plans ADD COLUMN completion_error_log TEXT DEFAULT '[]';
```

### 2. Implement `agent_plan_complete()`

New function in `agent_plan.c`:

```c
int agent_plan_complete(app_ctx_t *ctx, int plan_id);
```

The completion loop:
1. Load the plan and its steps
2. Find the first failed or pending step
3. Delegate the step (using existing delegation routing)
4. Run the verify command if configured
5. If verify passes and no failed steps remain → mark plan as `complete`
6. If verify fails → increment iteration, record the error
7. If the same error appears 3 times → stop and escalate with diagnostic
8. If iteration >= max → stop and report partial progress
9. Persist state after each iteration

### 3. Error deduplication and circuit breaking

Track errors in `completion_error_log` (JSON array). Before retrying, check if the same error has appeared before:

```c
typedef struct {
    int iteration;
    char error_signature[256];  // first 256 chars of error, normalized
    char step_action[256];
} completion_error_t;
```

If the same `error_signature` appears 3 times:
- Stop the loop
- Delegate a `reason` task: "This error has recurred 3 times. Analyze root cause and suggest a different approach."
- Present the analysis to the user/primary agent

### 4. CLI and MCP interface

```bash
aimee plan complete <plan_id>                     # start completion loop
aimee plan complete <plan_id> --verify "make test" # with verification
aimee plan complete <plan_id> --max-iter 10        # custom iteration cap
```

MCP tool:
```json
{
  "name": "plan_complete",
  "description": "Execute a plan in persistent completion mode with retry and verification",
  "parameters": {
    "plan_id": "integer",
    "verify_cmd": "string (optional)",
    "max_iterations": "integer (default 5)"
  }
}
```

### 5. Integration with consensus planning

If the consensus-planning proposal is also implemented, the completion loop can trigger a re-review when the plan is modified during retry. This closes the loop: plan → review → execute → verify → (retry if needed) → done.

### Changes

| File | Change |
|------|--------|
| `src/agent_plan.c` | Add `agent_plan_complete()` with iteration loop, error tracking, circuit breaker |
| `src/headers/agent_plan.h` | Add completion types and constants |
| `src/cmd_agent_plan.c` | Add `plan complete` subcommand |
| `src/mcp_tools.c` | Add `plan_complete` MCP tool |
| `src/db.c` | Add migration for completion columns |
| `src/tests/test_plan_complete.c` | Tests for completion loop, error dedup, circuit breaker |

## Acceptance Criteria

- [ ] `aimee plan complete <id>` iterates until all steps pass or max iterations reached
- [ ] Verification command runs after each iteration when configured
- [ ] Same error 3x triggers circuit breaker with root-cause delegation
- [ ] Completion state persists in DB across sessions
- [ ] `plan_complete` MCP tool is callable by the primary agent
- [ ] Partial progress is reported when the loop is capped

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (3-4 focused sessions)
- **Dependencies:** None (composes with consensus-planning but doesn't require it)

## Rollout and Rollback

- **Rollout:** New columns with defaults. Completion mode is opt-in per plan.
- **Rollback:** Revert commit. Columns are additive. Existing plans unaffected.
- **Blast radius:** Low. Only affects plans explicitly put in completion mode.

## Test Plan

- [ ] Unit tests: completion loop with mock delegates — all-pass, retry-then-pass, circuit-breaker
- [ ] Unit tests: error signature deduplication
- [ ] Unit tests: verify command integration
- [ ] Integration tests: end-to-end plan create → complete → verified done
- [ ] Failure injection: delegate unavailable mid-loop → graceful stop with state preserved
- [ ] Manual verification: create a plan with a failing step, observe retry and eventual completion

## Operational Impact

- **Metrics:** `completion_loops_started`, `completion_iterations_total`, `completion_circuit_breaks`, `completion_loops_succeeded`
- **Logging:** Per-iteration progress: `aimee: plan #N complete: iter 2/5, 3/5 steps done, verify: FAIL`
- **Alerts:** None
- **Disk/CPU/Memory:** 1+ delegate call per iteration. Error log grows ~200 bytes per error.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Core completion loop | P1 | M | High — primary value |
| Error circuit breaker | P1 | S | High — prevents runaway spend |
| Verify command integration | P1 | S | High — verifiable completion |
| CLI command | P2 | S | Medium |
| MCP tool | P2 | S | Medium |

## Trade-offs

**Why cap at 5 iterations by default?**
Most tasks either succeed within 3 attempts or have a fundamental issue. 5 gives a buffer. The cap is configurable for tasks known to need more iterations.

**Why not retry individual steps in parallel?**
Steps often have dependencies (step 2 depends on step 1's output). Sequential retry is safer. For independent steps, the delegate can parallelize internally.

**Why error signature matching instead of exact match?**
Error messages often include timestamps, line numbers, or variable values that differ between runs. Normalizing to a signature (first 256 chars, stripped of numbers) catches recurring root causes.
