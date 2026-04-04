# Proposal: Evidence-Based Completion Verification

## Problem

When aimee's primary agent or a delegate declares a task "done," there is no verification that the claim is backed by concrete evidence. The agent says "I fixed the bug" but there may be no test proving it, no diff showing the change, and no command output confirming the fix.

Aimee's `git_verify.c` runs build/test/lint gates — which checks that the *build* is healthy — but not that the *task's acceptance criteria* are satisfied. A plan step can be marked `done` without proof that it actually accomplished what it was supposed to.

oh-my-codex's verifier role enforces evidence-based completion: every claim must be backed by a concrete artifact (diff, test output, command result, screenshot). The verifier distinguishes between "missing evidence" (needs more investigation) and "actual failure" (the thing doesn't work), preventing premature closure.

Evidence:
- `agent_plan.c` marks steps as `done` based on the delegate's self-report
- No verification that the step's `success_predicate` was actually checked
- Plan steps have `success_predicate` fields but nothing enforces running them
- The completion loop proposal adds retry logic but doesn't add evidence collection

## Goals

- Plan step completion requires running the step's `success_predicate` and recording the output
- Delegates that claim completion without evidence are flagged
- A verification pass can be run on any completed plan to check all success predicates
- Evidence (command output, diffs, test results) is stored alongside the step for auditability

## Approach

### 1. Evidence storage

Add an evidence table linked to plan steps:

```sql
CREATE TABLE IF NOT EXISTS step_evidence (
    id INTEGER PRIMARY KEY,
    step_id INTEGER NOT NULL REFERENCES plan_steps(id),
    kind TEXT NOT NULL,  -- predicate_result, diff, test_output, command_output, delegate_claim
    content TEXT NOT NULL,
    passed INTEGER,  -- 1=pass, 0=fail, NULL=informational
    created_at TEXT DEFAULT (datetime('now'))
);
```

### 2. Automatic predicate execution

When a plan step is marked `done` (in `agent_plan_step_done()`), automatically run its `success_predicate` if one exists:

```c
int agent_plan_step_verify(app_ctx_t *ctx, int plan_id, int step_seq);
```

1. Load the step's `success_predicate` (e.g., `"make test-memory"`, `"grep -q 'workflow' src/headers/memory_types.h"`)
2. Execute it via subprocess
3. Record the result as `predicate_result` evidence
4. If the predicate fails → revert step status to `failed`, record the output
5. If no predicate exists → record a `delegate_claim` evidence entry (weaker evidence)

### 3. Evidence classification

Evidence has different strengths:

| Kind | Strength | Description |
|------|----------|-------------|
| `predicate_result` | Strong | Success predicate command passed |
| `test_output` | Strong | Test suite output showing new/fixed test |
| `diff` | Medium | Git diff showing the change was made |
| `command_output` | Medium | Arbitrary command showing expected state |
| `delegate_claim` | Weak | Delegate said it's done (no verification) |

### 4. Verification pass

A full verification re-runs all success predicates for a plan:

```bash
aimee plan verify <plan_id>       # re-run all success predicates
```

Output:
```
Plan #12: 7 steps
  Step 1: [PASS] predicate: make test-memory (exit 0)
  Step 2: [PASS] predicate: grep -q 'workflow' src/headers/memory_types.h (exit 0)
  Step 3: [WEAK] no predicate — delegate claim only
  Step 4: [FAIL] predicate: make lint (exit 1, 2 warnings)
  ...
Verdict: 5 passed, 1 failed, 1 weak (no predicate)
```

### 5. Integration with completion loop

If the persistent-completion-loop proposal is implemented, the completion loop should:
1. After a step is delegated and claimed done → run `agent_plan_step_verify()`
2. If verification fails → the step stays `failed` and the loop retries it
3. Evidence accumulates across iterations, providing an audit trail

### 6. MCP tool

```json
{
  "name": "verify_step",
  "description": "Run the success predicate for a plan step and record evidence",
  "parameters": {
    "plan_id": "integer",
    "step_seq": "integer"
  }
}
```

### Changes

| File | Change |
|------|--------|
| `src/agent_plan.c` | Add `agent_plan_step_verify()`, auto-verify on step completion |
| `src/db.c` | Add `step_evidence` table migration |
| `src/cmd_agent_plan.c` | Add `plan verify` subcommand |
| `src/mcp_tools.c` | Add `verify_step` MCP tool |
| `src/tests/test_step_evidence.c` | Tests for predicate execution, evidence storage, verification pass |

## Acceptance Criteria

- [ ] Completing a plan step with a `success_predicate` automatically runs the predicate
- [ ] Predicate failure reverts the step to `failed` status
- [ ] Evidence is stored in `step_evidence` with kind, content, and pass/fail
- [ ] `aimee plan verify <id>` re-runs all predicates and reports results
- [ ] Steps without predicates are flagged as `weak` (delegate claim only)
- [ ] `verify_step` MCP tool is callable

## Owner and Effort

- **Owner:** aimee
- **Effort:** S-M (2-3 focused sessions)
- **Dependencies:** Existing plan IR. Composes with completion-loop proposal.

## Rollout and Rollback

- **Rollout:** New table. Auto-verification on step completion is additive — steps without predicates behave as before.
- **Rollback:** Revert commit. Drop table. Steps complete without verification (current behavior).
- **Blast radius:** Low. Only affects plan steps that have `success_predicate` defined.

## Test Plan

- [ ] Unit tests: predicate execution — pass, fail, timeout, missing
- [ ] Unit tests: evidence storage and retrieval
- [ ] Unit tests: step status revert on predicate failure
- [ ] Integration tests: end-to-end plan step → delegate → verify → evidence stored
- [ ] Failure injection: predicate command hangs → timeout after 30s → step marked failed
- [ ] Manual verification: create a plan with predicates, complete steps, run `plan verify`

## Operational Impact

- **Metrics:** `step_verifications_run`, `step_verifications_passed`, `step_verifications_failed`, `steps_with_weak_evidence`
- **Logging:** `aimee: plan #12 step 3: predicate PASS (make test-memory, exit 0, 1.2s)`
- **Alerts:** None
- **Disk/CPU/Memory:** One subprocess per predicate. Evidence content stored as text (~1KB per entry).

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Auto-verify on step completion | P1 | S | High — core evidence collection |
| Evidence storage | P1 | S | High — audit trail |
| Verification pass command | P1 | S | High — re-verification |
| Completion loop integration | P2 | S | Medium — composes with retry |
| MCP tool | P2 | S | Medium |

## Trade-offs

**Why predicates as shell commands instead of structured checks?**
Shell commands are universal — `make test`, `grep -q`, `curl -sf`. Any verification that can be expressed as "exit 0 = pass" works. Structured checks would need a DSL that's less flexible.

**Why not require predicates on all steps?**
Some steps are inherently hard to verify automatically (e.g., "refactor for clarity"). Requiring predicates would force authors to write meaningless checks. Instead, flag predicate-less steps as `weak` so the user knows where evidence is missing.

**Why store evidence in a separate table instead of in the step's output field?**
A step may accumulate multiple pieces of evidence across iterations (attempt 1 failed, attempt 2 passed). A separate table with foreign key supports this history. The existing `output` field is a single text blob.
