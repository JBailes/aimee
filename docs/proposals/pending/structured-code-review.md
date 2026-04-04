# Proposal: Verification, Review, and Completion Pipeline

## Problem

The pending set currently splits acceptance into separate proposals:

- delegates claim completion too early
- plan steps are marked done without evidence
- review is unstructured
- parallel review is treated as a separate subsystem
- retry-until-done logic is separated from verification

In practice these are one workflow. Work should not be considered done until it has:

1. a completion claim
2. evidence
3. review
4. an accept/reject decision
5. optional retry with failure context

Keeping these as separate proposals duplicates schema and leaves the final acceptance boundary ambiguous.

## Goals

- Delegate completion claims trigger verification instead of being trusted blindly.
- Plan steps collect evidence and run success predicates where available.
- Review findings are structured, categorized, and stored.
- Multi-perspective review is part of the same verification pipeline.
- Blocking failures can feed a completion loop rather than forcing manual orchestration.

## Approach

Implement one verification pipeline with four layers:

1. completion-claim verification prompting/gating
2. evidence collection and predicate execution
3. structured multi-perspective review
4. optional persistent completion loop

### Evidence and Findings Model

```sql
CREATE TABLE IF NOT EXISTS step_evidence (
    id INTEGER PRIMARY KEY,
    step_id INTEGER NOT NULL REFERENCES plan_steps(id),
    kind TEXT NOT NULL,
    content TEXT NOT NULL,
    passed INTEGER,
    created_at TEXT DEFAULT (datetime('now'))
);
```

```sql
CREATE TABLE IF NOT EXISTS review_findings (
    id INTEGER PRIMARY KEY,
    session_id TEXT NOT NULL,
    plan_id INTEGER,
    category TEXT NOT NULL,
    severity TEXT NOT NULL,
    file_path TEXT,
    line_number INTEGER,
    description TEXT NOT NULL,
    suggestion TEXT,
    reviewer TEXT,
    status TEXT DEFAULT 'open',
    created_at TEXT DEFAULT (datetime('now'))
);
```

### Completion Claim Verification

When a delegate reports completion, inject a concise verification protocol into the orchestrator:

- read changed files before trusting the summary
- run automated verification
- check scope against the original task
- refuse to mark work done without evidence

### Predicate Execution

When a plan step is marked done, run its `success_predicate` if present and record the output as evidence. Predicate failure reverts the step to a failed or contested state.

### Structured Review

Run configurable review perspectives in parallel, such as:

- security
- quality
- architecture

Each perspective produces structured findings with category, severity, file:line, description, and suggestion.

### Completion Loop

For plans running in completion mode:

1. delegate a pending or failed step
2. collect evidence and run predicates
3. run structured review if configured
4. if blocking failures remain, retry with recorded failure context
5. stop after repeated identical failures or max iterations

### CLI and MCP

```bash
aimee verify
aimee verify --review-only
aimee verify findings
aimee plan verify <plan_id>
aimee plan complete <plan_id>
```

MCP tools:

- `structured_review`
- `verify_step`
- `plan_complete`

### Changes

| File | Change |
|------|--------|
| `src/agent_coord.c` | Inject delegate-completion verification reminders |
| `src/agent_plan.c` | Step verification, evidence recording, and completion-loop orchestration |
| `src/git_verify.c` | Structured multi-perspective review |
| `src/db.c` | Add `step_evidence`, `review_findings`, and completion-loop schema |
| `src/mcp_tools.c` | Add `structured_review`, `verify_step`, and `plan_complete` tools |
| `src/cmd_core.c` | Extend verify and add plan verify/complete flows |

## Acceptance Criteria

- [ ] Delegate completion claims trigger verification guidance before the orchestrator proceeds.
- [ ] Plan steps with `success_predicate` automatically record evidence and revert on failure.
- [ ] `aimee verify` runs configured review perspectives in parallel alongside build/test/lint.
- [ ] Findings are stored with category, severity, and file references.
- [ ] `aimee plan verify <id>` re-runs predicates and reports weak vs strong evidence.
- [ ] `aimee plan complete <id>` retries until acceptance or configured limits are hit.

## Owner and Effort

- **Owner:** aimee
- **Effort:** L
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Ship in layers: completion-claim verification first, evidence capture second, structured review third, completion loop last.
- **Rollback:** Each layer can be disabled independently; the system should degrade to simpler verification rather than fail outright.
- **Blast radius:** Medium. This proposal changes acceptance semantics.

## Test Plan

- [ ] Unit tests: delegate completion protocol injection
- [ ] Unit tests: predicate execution and evidence storage
- [ ] Unit tests: finding parsing and severity gating
- [ ] Unit tests: completion-loop retry and repeated-error circuit breaker
- [ ] Integration tests: plan step done → predicate verify → review → retry/final verdict

## Operational Impact

- **Metrics:** `step_verifications_run`, `steps_with_weak_evidence`, `review_findings_total`, `completion_loops_started`, `completion_circuit_breaks`
- **Logging:** Per-step verification and per-perspective review summaries
- **Alerts:** None
- **Disk/CPU/Memory:** Additional subprocesses for predicates and delegate calls for review

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Completion-claim verification | P1 | S | High |
| Evidence capture + predicate execution | P1 | S | High |
| Structured multi-perspective review | P1 | M | High |
| Completion loop integration | P1 | M | High |

## Trade-offs

- **Why merge review, evidence, and completion?** Acceptance is one workflow.
- **Why parallel review?** Security, quality, and architecture review are independent enough to parallelize.
- **Why store evidence and findings?** They must survive retries and support auditability.
