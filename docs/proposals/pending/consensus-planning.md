# Proposal: Consensus Planning via Multi-Delegate Review

## Problem

Aimee's `plan` mode blocks file writes but does not improve plan quality. The agent creates a plan, and the user either approves or rejects it. There is no structured adversarial review before implementation begins.

This means:
- Plans are single-perspective: the primary agent both designs and self-approves
- Architectural flaws surface during implementation, wasting tokens and requiring rework
- The existing delegate infrastructure (reviewer, critic roles) goes unused during planning
- The plan IR (`execution_plans` / `plan_steps` tables) stores plans but has no `reviewed` or `contested` state

oh-my-codex's `$ralplan` skill demonstrates the value of multi-perspective consensus: a Planner proposes, an Architect stress-tests for soundness, and a Critic evaluates against quality criteria. The result is plans that survive adversarial review *before* any code is written.

Evidence:
- `agent_plan.c` creates plans and tracks step status but has no review/approval workflow
- `cmd_hooks.c` enforces plan mode by blocking writes, but doesn't route the plan through delegates
- Delegation roles `review` and `reason` exist and route to configured delegates but are never used during planning

## Goals

- Plans created in plan mode are automatically reviewed by 2+ delegate perspectives (architect/reviewer, critic) before the primary agent proceeds to implementation
- Review is sequential: architect review completes before critic review begins (critic needs architect's assessment as input)
- The plan IR records review verdicts and the reviewing delegate
- Users can opt into interactive approval gates at key decision points
- The feature composes with existing delegation routing — uses the cheapest suitable delegate per role

## Approach

### 1. Add plan review states to the plan IR

Extend the `execution_plans` table with review tracking:

```sql
ALTER TABLE execution_plans ADD COLUMN review_status TEXT DEFAULT 'unreviewed';
-- values: unreviewed, reviewing, approved, contested, rejected
ALTER TABLE execution_plans ADD COLUMN review_log TEXT DEFAULT '[]';
-- JSON array of {role, delegate, verdict, reasoning, timestamp}
ALTER TABLE execution_plans ADD COLUMN review_iteration INTEGER DEFAULT 0;
```

Add constants in `headers/agent_plan.h`:

```c
#define PLAN_REVIEW_UNREVIEWED  "unreviewed"
#define PLAN_REVIEW_REVIEWING   "reviewing"
#define PLAN_REVIEW_APPROVED    "approved"
#define PLAN_REVIEW_CONTESTED   "contested"
#define PLAN_REVIEW_REJECTED    "rejected"
#define PLAN_REVIEW_MAX_ITER    3
```

### 2. Implement `agent_plan_review()`

New function in `agent_plan.c` that orchestrates the review cycle:

```c
int agent_plan_review(app_ctx_t *ctx, int plan_id, int interactive);
```

The function:
1. Loads the plan from the DB
2. Formats the plan as a review prompt (task description, steps with preconditions/predicates)
3. Delegates to `review` role: "Review this plan for architectural soundness. Identify the strongest counterargument. Score: approve / contest / reject."
4. Waits for the review result
5. Delegates to `reason` role (or `review` if `reason` unavailable): "Given this plan and the architect's review, evaluate against: completeness, testability, blast radius, rollback safety. Score: approve / contest / reject."
6. Records both verdicts in `review_log`
7. If both approve → set `review_status = 'approved'`
8. If either contests → set `review_status = 'contested'`, increment `review_iteration`
9. If iteration < max → return contested status so the primary agent can revise
10. If iteration >= max → present best version to user for manual decision

### 3. Integrate with plan mode flow

In `cmd_hooks.c`, when plan mode is active and a plan is created (PostToolUse detects plan creation), automatically trigger `agent_plan_review()`. The session context should include review verdicts so the primary agent sees feedback.

Modify `build_session_context()` to include a "# Plan Review" section when a plan exists with non-approved status:

```
# Plan Review (iteration 2/3, status: contested)
Architect: Contest — step 3 modifies shared state without a lock; rollback step is incomplete.
Critic: Approve — scope and testability are adequate.
→ Revise plan to address architect's concerns before implementing.
```

### 4. Add `aimee plan review` CLI command

For manual invocation outside of auto-review:

```bash
aimee plan review <plan_id>           # review latest plan
aimee plan review <plan_id> --interactive  # pause for user input at each gate
```

### 5. MCP tool: `review_plan`

Expose plan review through MCP so the primary agent can explicitly request it:

```json
{
  "name": "review_plan",
  "description": "Submit a plan for multi-delegate consensus review",
  "parameters": {
    "plan_id": "integer",
    "interactive": "boolean (default false)"
  }
}
```

### Changes

| File | Change |
|------|--------|
| `src/agent_plan.c` | Add `agent_plan_review()` with sequential delegate review loop |
| `src/headers/agent_plan.h` | Add review status constants, review function declaration |
| `src/cmd_hooks.c` | Auto-trigger plan review on plan creation in plan mode; add review section to session context |
| `src/cmd_agent_plan.c` | Add `plan review` subcommand |
| `src/mcp_tools.c` | Add `review_plan` MCP tool |
| `src/db.c` | Add migration for `review_status`, `review_log`, `review_iteration` columns |
| `src/tests/test_plan_review.c` | Tests for review cycle, iteration limits, verdict recording |

## Acceptance Criteria

- [ ] `aimee plan review <id>` routes the plan to architect (review role) then critic (reason role) sequentially
- [ ] Review verdicts are stored in `review_log` JSON column
- [ ] Contested plans increment `review_iteration` and cap at 3
- [ ] Session context includes plan review status and delegate feedback
- [ ] `review_plan` MCP tool is callable by the primary agent
- [ ] Review uses existing delegate routing — cheapest suitable delegate per role
- [ ] Fallback: if no review-capable delegate is configured, plan review is skipped with a warning (not blocking)
- [ ] All existing tests pass; new tests cover the review cycle

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (3-4 focused sessions)
- **Dependencies:** None — builds on existing plan IR and delegation infrastructure

## Rollout and Rollback

- **Rollout:** DB migration adds columns with defaults, so existing plans are unaffected. Auto-review in plan mode can be gated behind `aimee config set plan.auto_review true`.
- **Rollback:** Revert commit. Migration columns are additive. Existing plan creation/execution is unchanged.
- **Blast radius:** Low. Only affects plan mode. Plans without review remain fully functional.

## Test Plan

- [ ] Unit tests: `agent_plan_review` with mock delegate responses — approve/approve, contest/approve, reject/approve
- [ ] Unit tests: review iteration capping at max
- [ ] Unit tests: review_log JSON serialization and deserialization
- [ ] Integration tests: end-to-end plan create → auto-review → session context shows verdict
- [ ] Failure injection: delegate unavailable → graceful skip with warning
- [ ] Manual verification: create a plan in plan mode, observe review feedback in context

## Operational Impact

- **Metrics:** `plan_reviews_triggered`, `plan_review_verdicts{verdict=approved|contested|rejected}`, `plan_review_iterations`
- **Logging:** Review cycle progress to stderr: `aimee: plan #N review: architect=approve, critic=contest (iter 1/3)`
- **Alerts:** None
- **Disk/CPU/Memory:** 2 delegate calls per review cycle. Each review adds ~200 bytes to review_log.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Plan review function + DB schema | P1 | M | High — core value |
| Auto-review in plan mode | P1 | S | High — makes it seamless |
| Session context integration | P1 | S | High — closes the feedback loop |
| CLI command | P2 | S | Medium — manual escape hatch |
| MCP tool | P2 | S | Medium — agent-callable |

## Trade-offs

**Why not parallel architect + critic reviews?**
The critic's value comes from evaluating *in light of* the architect's assessment. Parallel reviews would produce two independent opinions rather than a layered critique. Sequential is worth the extra latency.

**Why cap at 3 iterations instead of converging?**
Unbounded loops risk infinite delegate spend on plans that fundamentally won't converge. 3 iterations is enough to surface real issues; beyond that, human judgment is needed.

**Why not add new delegate roles (architect, critic)?**
The existing `review` and `reason` roles are sufficient. Adding roles would require users to reconfigure delegates. The review prompt can specify the perspective without a dedicated role.

**Why not require review for all plans?**
Some plans are trivial. Auto-review is configurable. The MCP tool lets the agent request review when it judges the plan warrants it.
