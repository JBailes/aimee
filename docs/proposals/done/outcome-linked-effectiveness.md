# Proposal: Outcome-Linked Memory Effectiveness Tracking

## Problem

Memory use_count tracks how often a memory was *surfaced*, not whether it *helped*. A memory included in context 50 times that never correlates with successful outcomes is noise, not knowledge. Without a feedback signal, the system cannot distinguish useful memories from harmful ones, and every optimization (better retrieval, smarter compaction, embeddings) is ungrounded.

The system tracks:
- `use_count`: how many times a memory was included in context
- `confidence`: a static score, only adjusted by contradictions or manual override
- `last_used_at`: recency of access

None of these capture: "Was this memory present when the agent succeeded? Was it absent when the agent failed? Did surfacing this memory correlate with better outcomes?"

Evidence: `memory_touch()` in `memory.c` increments `use_count` unconditionally. No downstream signal flows back to the memory that was surfaced. The agent_log tracks delegation success/failure but doesn't link back to which memories were in context.

## Goals

- Track which memories were included in context for each session/delegation.
- Correlate memory presence with session outcomes (success, failure, rule violations).
- Use effectiveness scores to influence retrieval ranking and lifecycle decisions.
- Surface low-effectiveness memories for operator review.

## Approach

### 1. Context Snapshot Table

Record which memories were included in each context assembly:

```sql
CREATE TABLE context_snapshots (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    memory_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,
    relevance_score REAL,     /* score at assembly time */
    created_at TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX idx_ctx_snap_session ON context_snapshots(session_id);
CREATE INDEX idx_ctx_snap_memory ON context_snapshots(memory_id);
```

Populated during `memory_assemble_context()` — for each memory included in the output, insert a row.

### 2. Outcome Recording

Extend the session lifecycle to record outcomes:

```sql
ALTER TABLE sessions ADD COLUMN outcome TEXT DEFAULT NULL;
  /* 'success', 'failure', 'partial', 'unknown' */
ALTER TABLE sessions ADD COLUMN rule_violations INTEGER DEFAULT 0;
```

Outcome is determined at session end:
- **success:** delegation succeeded (`agent_log.success=1`), no rule violations
- **failure:** delegation failed, or session ended with unresolved errors
- **partial:** delegation succeeded but with rule violations
- **unknown:** no delegation (interactive session), outcome not determinable

### 3. Effectiveness Score Computation

A periodic job (run during maintenance) computes per-memory effectiveness:

```c
typedef struct {
    int64_t memory_id;
    int times_surfaced;      /* total context inclusions */
    int success_present;     /* times present during successful sessions */
    int failure_present;     /* times present during failed sessions */
    double effectiveness;    /* computed score */
} memory_effectiveness_t;
```

Formula:

```
effectiveness = (success_present + 1) / (times_surfaced + 2)  /* Laplace smoothing */
```

This gives a score between 0 and 1, where:
- A memory surfaced 10 times with 8 successes: `9/12 = 0.75`
- A memory surfaced 10 times with 2 successes: `3/12 = 0.25`
- A never-surfaced memory: `1/2 = 0.5` (neutral prior)

### 4. Effectiveness-Informed Retrieval

Integrate effectiveness into the retrieval scoring formula:

```
final_score = alpha * relevance + beta * effectiveness + gamma * confidence

alpha = 0.5, beta = 0.3, gamma = 0.2
```

Memories with low effectiveness are naturally deprioritized without being deleted — they may still be useful in different contexts.

### 5. Effectiveness-Informed Lifecycle

Add effectiveness to the demotion criteria:

```c
/* Current: demote if unused for N days AND confidence < threshold */
/* Proposed: also demote if effectiveness < 0.3 AND surfaced >= 10 times */
if (effectiveness < 0.3 && times_surfaced >= 10) {
    /* This memory is actively harmful — it's surfaced often but doesn't help */
    /* Demote regardless of recency or confidence */
}
```

This catches memories that are retrieved frequently (high use_count) but don't contribute to success.

### 6. Observability

Add to `aimee memory stats`:

```
Memory Effectiveness:
  Avg effectiveness:     0.68
  Low-effectiveness (>10 uses, <0.3 score): 3
  Never-surfaced L2:     12
  High-impact (>10 uses, >0.8 score): 8
```

Add `aimee memory list --low-effectiveness` to surface candidates for review.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | New migration: `context_snapshots` table, `outcome` column on sessions |
| `src/memory_context.c` | Record context snapshots during assembly |
| `src/memory_promote.c` | `memory_compute_effectiveness()`, effectiveness-informed demotion |
| `src/memory.c` | Effectiveness score in search ranking |
| `src/cmd_hooks.c` | Record session outcome at session end |
| `src/cmd_memory.c` | `--low-effectiveness` flag, effectiveness in stats |
| `src/headers/memory.h` | `memory_effectiveness_t` struct |
| `src/headers/aimee.h` | `EFFECTIVENESS_DEMOTE_THRESHOLD 0.3`, `EFFECTIVENESS_MIN_SAMPLES 10` |

## Acceptance Criteria

- [ ] Context assembly records which memories were included per session
- [ ] Session outcomes (success/failure/partial) are recorded
- [ ] Effectiveness scores are computed during maintenance
- [ ] Low-effectiveness memories (>10 uses, <0.3 score) are flagged for demotion
- [ ] Retrieval scoring incorporates effectiveness
- [ ] `aimee memory stats` shows effectiveness metrics
- [ ] `aimee memory list --low-effectiveness` shows candidates for review
- [ ] Effectiveness computation completes in <100ms for 1000 memories

## Owner and Effort

- **Owner:** TBD
- **Effort:** L
- **Dependencies:** None, but benefits from task-aware assembly and retrieval planner

## Rollout and Rollback

- **Rollout:** Migration adds `context_snapshots` table and `outcome` column. Effectiveness scores are NULL until enough sessions accumulate data. Scoring formula uses neutral prior (0.5) for unscored memories.
- **Rollback:** Revert commit. `context_snapshots` table remains but is unused. Scoring reverts to confidence + use_count.
- **Blast radius:** Changes retrieval ranking based on observed effectiveness. New memories start at neutral (0.5), so no immediate change. Effect builds over time as data accumulates.

## Test Plan

- [ ] Unit test: context assembly inserts snapshot rows for included memories
- [ ] Unit test: effectiveness formula returns correct values for known inputs
- [ ] Unit test: Laplace smoothing gives 0.5 for no data
- [ ] Unit test: memory with 10 surfaces and 2 successes scores 0.25
- [ ] Unit test: low-effectiveness memory is flagged for demotion
- [ ] Unit test: effectiveness affects search ranking
- [ ] Integration test: run 5 successful sessions, verify effectiveness rises for surfaced memories
- [ ] Manual: `aimee memory stats` shows effectiveness section after 10+ sessions

## Operational Impact

- **Metrics:** Avg effectiveness, low-effectiveness count, high-impact count in stats.
- **Logging:** Effectiveness computation logged at debug. Low-effectiveness detections at info.
- **Alerts:** None.
- **Disk/CPU/Memory:** `context_snapshots` grows ~16 rows per session (MAX_CONTEXT_MEMS). At 100 sessions/month = 1600 rows/month. Negligible. Periodic cleanup can archive snapshots older than 90 days.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Context snapshots | P0 | S | Foundation — must capture data first |
| Outcome recording | P0 | S | Foundation |
| Effectiveness computation | P1 | M | Core value |
| Retrieval integration | P1 | S | Closes the feedback loop |
| Lifecycle integration | P2 | S | Prunes harmful memories |
| Observability | P2 | S | Operator visibility |

## Trade-offs

**Why Laplace smoothing instead of raw ratios?** Raw ratios are unstable with small sample sizes — a memory surfaced once in a successful session gets 1.0, which is meaningless. Laplace smoothing (`(successes + 1) / (trials + 2)`) provides a conservative estimate that converges to the true rate as data accumulates. The neutral prior (0.5) means new memories aren't penalized.

**Why not attribute causation?** We can't know if a memory *caused* success — only that it was *present* during success. This is a correlation signal, not causal inference. But correlation is still useful: a memory that's consistently present during failures and absent during successes is, at minimum, not helping and is likely correlated with the wrong retrieval contexts.

**Why 10 as the minimum sample size?** Below 10 observations, the Laplace-smoothed estimate is still dominated by the prior. Demotion based on fewer samples would be premature. 10 sessions gives enough signal to distinguish a 0.3 from a 0.7 with reasonable confidence.
