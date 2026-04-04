# Proposal: Delegation Robustness — Retry, Checkpoints, and Timeouts

## Problem

When a delegation fails mid-way through a multi-step task, all progress is lost.
The coordinator (`agent_coord.c`) has no mechanism to:

1. **Recover partial progress.** A delegation that completed 3 of 5 file edits
   before hitting an error loses the context of what succeeded. The retry starts
   from scratch with no memory of prior work.
2. **Retry with context.** When `--retry N` is specified, each retry attempt is
   identical to the original — it does not include information about what failed
   or what was already done.
3. **Enforce timeouts.** The only timeout is implicit: if the compute pool queue
   is full (`compute_pool_submit()` returns -1), the delegation is rejected. A
   delegation that hangs indefinitely (e.g., sub-agent stuck waiting for
   provider response) ties up a worker thread with no cancellation.

## Goals

- Failed delegations record what succeeded and what failed.
- Retry attempts receive failure context so sub-agents can resume, not restart.
- Per-delegation wall-clock timeout with graceful cancellation.

## Approach

### 1. Progress checkpointing

Add a `delegation_checkpoint` table:

```sql
CREATE TABLE IF NOT EXISTS delegation_checkpoint (
    delegation_id TEXT PRIMARY KEY,
    job_id TEXT,
    steps_completed TEXT,   /* JSON array of completed step descriptions */
    last_output TEXT,       /* last successful output chunk */
    error TEXT,             /* error that caused failure */
    failed_at INTEGER,
    created_at INTEGER NOT NULL
);
```

During delegation, the sub-agent's tool calls and outputs are already logged in
`agent_log`. The checkpoint extracts a summary of completed work from the log
when a delegation fails: which tool calls succeeded, what output was produced,
and what the error was.

Location: `agent_coord.c` — after delegation failure, before recording the
outcome, write a checkpoint row summarizing completed steps.

### 2. Context-aware retry

When `--retry N` triggers a retry, the delegation payload includes a
`prior_attempt` field:

```json
{
    "role": "code",
    "prompt": "Add error handling to src/memory.c",
    "prior_attempt": {
        "attempt": 1,
        "steps_completed": ["read src/memory.c", "identified 3 error paths"],
        "error": "provider timeout after 30s",
        "last_output": "Started editing memory_search()..."
    }
}
```

The sub-agent sees what was already done and can resume from that point instead
of starting over. This is injected into the delegation prompt as context, not as
a hard constraint — the agent decides whether to resume or restart.

Location: `agent.c` — in the retry path, query `delegation_checkpoint` for the
prior attempt and include it in the payload.

### 3. Per-delegation timeout

Add a `--timeout <seconds>` flag to `aimee delegate` (default: 300s for
foreground, 600s for background). Implementation:

```c
/* In the compute pool worker thread */
static void *worker_thread(void *arg) {
    /* ... dequeue work item ... */
    if (item->timeout_ms > 0) {
        /* Set a timer that sends SIGALRM or sets a cancellation flag */
        item->deadline = clock_gettime_ms() + item->timeout_ms;
    }
    item->func(item->arg);
}
```

When the deadline expires:
1. Set a cancellation flag on the delegation context
2. The sub-agent HTTP client checks the flag between tool calls
3. If cancelled, return partial results and record checkpoint
4. Log timeout with delegation ID and elapsed time

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Add `delegation_checkpoint` table migration |
| `src/agent_coord.c` | Write checkpoint on failure, read checkpoint for retry context |
| `src/agent.c` | Include `prior_attempt` in retry payloads, add `--timeout` flag |
| `src/agent_jobs.c` | Enforce wall-clock deadline on background delegations |
| `src/compute_pool.c` | Add deadline field to work items, cancellation flag check |

## Acceptance Criteria

- [ ] Failed delegations create a checkpoint row with steps completed and error
- [ ] Retry attempts include `prior_attempt` context from checkpoint
- [ ] Sub-agents receive failure context and can choose to resume
- [ ] `--timeout` kills delegations that exceed deadline
- [ ] Timed-out delegations save checkpoint before termination
- [ ] `aimee trace` shows checkpoint data for failed delegations

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** DB migration adds checkpoint table. Retry context and timeout activate with new binary.
- **Rollback:** Revert commit. Retries revert to context-free behavior. Checkpoint table remains but unused.
- **Blast radius:** Retry behavior changes — sub-agents now see prior attempt info. If the context confuses the agent, retries may produce worse results. The agent can ignore the context if it decides to restart.

## Test Plan

- [ ] Unit test: checkpoint written on delegation failure with correct steps
- [ ] Unit test: retry payload includes prior_attempt from checkpoint
- [ ] Integration test: delegation with `--retry 1` passes failure context to retry
- [ ] Integration test: `--timeout 5` kills a hanging delegation after 5 seconds
- [ ] Integration test: timed-out delegation has checkpoint with partial progress

## Operational Impact

- **Metrics:** None.
- **Logging:** Checkpoint creation logged. Timeout events logged with delegation ID and elapsed time.
- **Alerts:** None.
- **Disk/CPU/Memory:** One checkpoint row per failed delegation (~1KB). Negligible.

## Priority

P1 — directly improves the core delegation workflow reliability.

## Trade-offs

**Why checkpoint summaries instead of full replay logs?** Full logs are already in
`agent_log`. The checkpoint is a concise summary for the sub-agent's context
window. Passing full logs would waste tokens on details the agent doesn't need.

**Why soft cancellation instead of SIGKILL?** Hard-killing a sub-agent process
prevents checkpoint writing. Soft cancellation (flag check between tool calls)
allows graceful shutdown with partial progress saved.
