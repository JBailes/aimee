# Proposal: Graceful Cancellation and Cleanup (`aimee cancel`)

## Problem

As aimee gains more stateful workflows (completion loops, jobs, pipelines, background delegations), there is no unified way to cancel in-flight work. Each subsystem would need its own cancel mechanism, and the user must know which one to invoke.

Currently:
- Background delegations have no cancel mechanism — they run to completion or failure
- Plan execution has no abort — a running plan step continues even if the plan is abandoned
- There is no cleanup for orphaned state (e.g., a crashed session leaving a plan in `running` status)

oh-my-codex's `$cancel` skill demonstrates unified mode-aware cancellation: it detects which modes are active, respects dependency ordering (autopilot → ralph → ultrawork → ecomode), performs mode-specific cleanup, and handles both graceful shutdown and forced cleanup.

Evidence:
- No `aimee cancel` command exists
- `execution_plans` rows can get stuck in `running` status if a session crashes
- Background delegations (`--background`) have no abort path
- No orphan detection or state cleanup mechanism

## Goals

- A single `aimee cancel` command stops all in-flight aimee workflows
- Cancellation is mode-aware — it knows what's running and cleans up appropriately
- Dependency ordering prevents half-torn-down state
- Orphan detection finds and cleans up stale state from crashed sessions
- A `--force` flag bypasses graceful shutdown for emergency cleanup

## Approach

### 1. Active workflow detection

```c
typedef struct {
    int has_active_pipeline;
    int pipeline_id;
    int has_active_job;
    int job_id;
    int has_active_plan;
    int plan_id;
    int background_delegates;  // count of in-flight background delegations
    char session_id[64];
} active_workflows_t;

int detect_active_workflows(app_ctx_t *ctx, active_workflows_t *out);
```

Query the DB for:
- `pipelines` with `status = 'active'`
- `jobs` with `status = 'running'`
- `execution_plans` with `status = 'running'` or steps in `running`
- Background task PIDs from the process table

### 2. Cancellation ordering

Cancel in dependency order (outermost orchestrator first):

```
pipeline → job → plan completion loop → plan execution → background delegates
```

Each level:
1. Set status to `cancelled` in the DB
2. If the workflow has a running subprocess (background delegate), send SIGTERM
3. Wait up to 5 seconds for graceful shutdown
4. If still running and `--force`, send SIGKILL

### 3. Orphan detection and cleanup

```bash
aimee cancel --orphans    # find and clean up stale state
```

Detect orphans:
- Plans in `running` status whose session ID doesn't match any active session
- Jobs with `running` tasks whose delegate process is not alive
- Pipelines in `active` status older than 24 hours with no recent updates

For each orphan:
- Set status to `cancelled` with a note: `"cancelled: orphan cleanup"`
- Log the cleanup

### 4. CLI interface

```bash
aimee cancel                    # cancel all active workflows (graceful)
aimee cancel --force            # cancel all + force-kill subprocesses
aimee cancel --orphans          # clean up stale state from crashed sessions
aimee cancel pipeline <id>      # cancel a specific pipeline
aimee cancel job <id>           # cancel a specific job
aimee cancel plan <id>          # cancel a specific plan execution
```

### 5. State preservation

Before cancellation, record what was cancelled and why:

```sql
ALTER TABLE execution_plans ADD COLUMN cancelled_at TEXT;
ALTER TABLE execution_plans ADD COLUMN cancel_reason TEXT;
```

Similarly for jobs and pipelines. This enables post-mortem analysis of what was in-flight when cancel was invoked.

### Changes

| File | Change |
|------|--------|
| `src/cmd_core.c` | Add `cancel` subcommand with workflow detection and ordered teardown |
| `src/agent_plan.c` | Add `agent_plan_cancel()` for plan-level cancellation |
| `src/agent_jobs.c` | Add `job_cancel()` for job-level cancellation (if coordinated-parallel-execution lands) |
| `src/agent_pipeline.c` | Add `pipeline_cancel()` (if autonomous-pipeline lands) |
| `src/db.c` | Add `cancelled_at`, `cancel_reason` columns to relevant tables |
| `src/tests/test_cancel.c` | Tests for detection, ordering, orphan cleanup |

## Acceptance Criteria

- [ ] `aimee cancel` detects all active workflows and cancels them in dependency order
- [ ] Cancelled workflows are marked `cancelled` in the DB with timestamp and reason
- [ ] `--force` kills subprocesses that don't respond to graceful shutdown within 5s
- [ ] `--orphans` finds and cleans up stale state from crashed sessions
- [ ] Specific cancel (`aimee cancel plan <id>`) works for individual workflows
- [ ] Cancel is idempotent — running it twice doesn't error

## Owner and Effort

- **Owner:** aimee
- **Effort:** S-M (2-3 focused sessions)
- **Dependencies:** Composes with pipeline, job, and completion-loop proposals but works with just plan IR

## Rollout and Rollback

- **Rollout:** New command. DB column additions are additive.
- **Rollback:** Revert commit. Stale state must be cleaned up manually (UPDATE in sqlite).
- **Blast radius:** None — cancel is user-initiated. Orphan cleanup only touches clearly stale state.

## Test Plan

- [ ] Unit tests: active workflow detection with various combinations
- [ ] Unit tests: cancellation ordering
- [ ] Unit tests: orphan detection heuristics
- [ ] Unit tests: idempotency — cancel already-cancelled workflow
- [ ] Integration tests: start a plan, cancel it, verify state is `cancelled`
- [ ] Failure injection: cancel with a dead subprocess — verify no hang

## Operational Impact

- **Metrics:** `cancel_invocations`, `cancel_orphans_cleaned`
- **Logging:** `aimee: cancel: pipeline #1 → job #3 → plan #12 (3 workflows cancelled)`
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible. DB queries + process signal delivery.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Active workflow detection | P1 | S | High — foundation |
| Ordered cancellation | P1 | S | High — prevents torn state |
| Orphan cleanup | P1 | S | High — operational hygiene |
| Per-workflow cancel | P2 | S | Medium — granularity |
| State preservation (cancelled_at) | P2 | S | Medium — auditability |

## Trade-offs

**Why dependency-ordered teardown instead of parallel cancel?**
A pipeline orchestrates a job which orchestrates plan steps. Cancelling a plan step while the job still thinks it's running creates inconsistent state. Top-down teardown ensures each layer sees consistent state.

**Why 5-second graceful timeout before force?**
Delegates may be mid-write. 5 seconds is enough for most delegates to finish their current operation. Longer risks the user waiting; shorter risks data loss.

**Why 24-hour threshold for orphan detection?**
Sessions can be long. A 24-hour threshold avoids false positives from active but slow work. Users can always force-cancel specific items if 24 hours is too generous.
