# Proposal: Autonomous End-to-End Pipeline (`aimee autopilot`)

## Problem

Aimee has individual building blocks — clarification, planning, delegation, verification — but no orchestrator that chains them into a complete autonomous pipeline. For multi-phase tasks (idea → spec → plan → implement → test → review → done), the primary agent must manually sequence these phases, deciding when to move from planning to implementation, when to run tests, and when to declare completion.

This manual orchestration wastes expensive primary-agent tokens on coordination logic and is error-prone (agents skip phases, forget to verify, or declare completion prematurely).

oh-my-codex's `$autopilot` skill demonstrates the value of a phased pipeline: Expansion → Planning → Execution → QA → Validation → Cleanup, with automatic transitions and circuit breakers at each phase.

Evidence:
- Aimee has `plan`, `delegate`, `verify`, and memory — all the building blocks
- No orchestrator chains these together
- The primary agent currently decides phase transitions ad-hoc
- The plan IR has step tracking but no pipeline/phase concept

## Goals

- A single command launches an autonomous pipeline from task description to verified completion
- The pipeline sequences through well-defined phases with automatic transitions
- Each phase has a circuit breaker (max iterations, error thresholds) to prevent runaway spend
- Phase state is persisted so the pipeline can resume after interruption
- The pipeline composes existing aimee features rather than reimplementing them

## Approach

### 1. Pipeline model

```sql
CREATE TABLE IF NOT EXISTS pipelines (
    id INTEGER PRIMARY KEY,
    task TEXT NOT NULL,
    status TEXT DEFAULT 'active',  -- active, paused, complete, failed, cancelled
    current_phase TEXT DEFAULT 'clarify',
    phase_data TEXT DEFAULT '{}',  -- JSON: per-phase state
    config TEXT DEFAULT '{}',  -- JSON: pipeline configuration
    clarification_id INTEGER,
    plan_id INTEGER,
    job_id INTEGER,
    created_at TEXT DEFAULT (datetime('now')),
    updated_at TEXT DEFAULT (datetime('now'))
);
```

### 2. Pipeline phases

The pipeline chains existing aimee features:

| Phase | Aimee feature used | Entry condition | Exit condition |
|-------|-------------------|-----------------|----------------|
| **Clarify** | `agent_clarify` (requirements-clarification proposal) | Task is vague (<50 words, no concrete anchors) | Ambiguity score < threshold |
| **Plan** | `agent_plan_create` | Spec available (from clarify or direct task) | Plan created with steps |
| **Review** | `agent_plan_review` (planning preparation/review pipeline) | Plan exists | Review status = approved |
| **Execute** | `job_execute` (coordinated-parallel proposal) or sequential delegation | Plan approved | All steps done |
| **QA** | `git_verify` + slop detection | Execution complete | Build + test + lint pass |
| **Validate** | `verify_structured_review` (structured-code-review proposal) | QA passes | No blocking findings |
| **Done** | — | Validation passes | Pipeline status = complete |

If the earlier proposals haven't been implemented yet, the pipeline degrades gracefully:
- No clarification? Skip clarify phase for all tasks
- No consensus planning? Skip review phase
- No job coordination? Execute steps sequentially via delegation
- No structured review? Run basic `aimee verify`

### 3. Phase transitions

```c
int pipeline_advance(app_ctx_t *ctx, int pipeline_id);
```

Each call advances the pipeline one phase:
1. Check current phase's exit condition
2. If met → transition to next phase
3. If not met → attempt the phase again (up to phase-specific max retries)
4. If max retries exceeded → pause the pipeline with diagnostic

The primary agent or a cron-like mechanism calls `pipeline_advance` to drive the pipeline forward.

### 4. Circuit breakers per phase

| Phase | Max retries | Circuit breaker |
|-------|------------|-----------------|
| Clarify | 20 rounds | Max rounds reached → proceed with partial spec |
| Plan | 3 | Can't produce a plan → fail |
| Review | 3 iterations | Max iterations → present to user |
| Execute | 5 per step | Same error 3x → escalate |
| QA | 5 cycles | Same failure 3x → halt |
| Validate | 3 rounds | Persistent critical findings → halt |

### 5. CLI and MCP

```bash
aimee autopilot "build a REST endpoint for user profiles"  # start pipeline
aimee autopilot status <id>                                 # show phase progress
aimee autopilot resume <id>                                 # resume a paused pipeline
aimee autopilot cancel <id>                                 # cancel
aimee autopilot --skip-clarify "concrete task"              # skip clarification
```

MCP:
```json
{
  "name": "autopilot_start",
  "description": "Launch an autonomous end-to-end pipeline for a task",
  "parameters": {
    "task": "string",
    "skip_phases": "array of strings (optional, phases to skip)"
  }
}
```

### Changes

| File | Change |
|------|--------|
| `src/agent_pipeline.c` | New: pipeline model, phase transitions, circuit breakers |
| `src/headers/agent_pipeline.h` | New: pipeline types and function declarations |
| `src/db.c` | Add `pipelines` table migration |
| `src/cmd_core.c` | Add `autopilot` subcommand |
| `src/mcp_tools.c` | Add `autopilot_start`, `autopilot_status` MCP tools |
| `src/tests/test_pipeline.c` | Tests for phase transitions, circuit breakers, graceful degradation |

## Acceptance Criteria

- [ ] `aimee autopilot "task"` starts a pipeline and advances through phases autonomously
- [ ] Phase state persists in DB — `aimee autopilot resume` continues where it left off
- [ ] Circuit breakers halt runaway phases
- [ ] Pipeline degrades gracefully when dependent proposals aren't implemented
- [ ] `aimee autopilot status` shows current phase and progress
- [ ] Each phase uses existing aimee infrastructure (no reimplementation)

## Owner and Effort

- **Owner:** aimee
- **Effort:** L (5-7 focused sessions)
- **Dependencies:** Composes with other proposals but works in degraded mode without them

## Rollout and Rollback

- **Rollout:** New table, new command. No changes to existing features.
- **Rollback:** Revert commit. Drop table. No impact on other features.
- **Blast radius:** None — entirely additive. The pipeline calls existing features; it doesn't modify them.

## Test Plan

- [ ] Unit tests: phase transition logic — advance, retry, circuit break
- [ ] Unit tests: graceful degradation — skip unavailable phases
- [ ] Unit tests: persistence — save and restore pipeline state
- [ ] Integration tests: end-to-end simple task → pipeline completes all phases
- [ ] Failure injection: delegate failure mid-execute → pipeline pauses, resume works
- [ ] Manual verification: run autopilot on a real task, observe phase progression

## Operational Impact

- **Metrics:** `pipelines_started`, `pipelines_completed`, `pipelines_failed`, `pipeline_phase_transitions`, `pipeline_circuit_breaks`
- **Logging:** Phase transitions: `aimee: pipeline #N: clarify → plan (score 0.18, 6 rounds)`
- **Alerts:** None
- **Disk/CPU/Memory:** Orchestration overhead is minimal — the real cost is in delegated phases.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Pipeline model + phase transitions | P1 | M | High — core orchestration |
| Circuit breakers | P1 | S | High — safety |
| Graceful degradation | P1 | S | High — works without all proposals |
| CLI interface | P2 | S | Medium |
| MCP tools | P2 | S | Medium |
| Resume/pause | P2 | S | Medium — session resilience |

## Trade-offs

**Why a separate pipeline table instead of extending execution_plans?**
A pipeline orchestrates multiple aimee features (clarification, planning, jobs, verification). The plan IR is one piece of the pipeline, not the whole thing. Extending it would conflate orchestration state with plan execution state.

**Why graceful degradation instead of hard dependencies?**
The proposals in this batch are independent. Users may implement some but not others. The pipeline should be useful even with just plan + delegate + verify (aimee's existing features).

**Why not make the pipeline fully autonomous (no pause/resume)?**
Some phases (e.g., clarification) require user input. Others may need human judgment when circuit breakers trigger. Pause/resume lets the pipeline hand off to the user and pick up again.
