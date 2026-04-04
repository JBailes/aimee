# Proposal: Memory Quality Metrics (Observability)

## Problem

The memory system uses simple confidence scores (0.0-1.0) and fixed thresholds
for promotion/demotion, but there is no visibility into how well the system is
performing — no metrics on contradiction frequency, promotion accuracy, or fact
staleness. High-confidence memories that are never used waste context budget;
silently decayed contradictions go unnoticed.

## Goals

- Operators can see whether the memory system is healthy (not just that it runs).
- Contradictions are visible and auditable, not silently resolved.

## Approach

### 1. Memory Health Metrics

Track per-maintenance-cycle metrics in a `memory_health` table:

```sql
CREATE TABLE memory_health (
    id INTEGER PRIMARY KEY,
    cycle_at TEXT NOT NULL,           -- ISO 8601 timestamp
    total_memories INTEGER NOT NULL,
    contradictions_detected INTEGER NOT NULL DEFAULT 0,
    promotions INTEGER NOT NULL DEFAULT 0,
    demotions INTEGER NOT NULL DEFAULT 0,
    expirations INTEGER NOT NULL DEFAULT 0
);
```

Populated by `memory_run_maintenance()` (`memory_promote.c:230`) which already
returns promoted/demoted/expired counts via out-parameters. Add contradiction
count tracking.

Retention: Keep 90 days of rows. Prune during maintenance. At one cycle per
session-start, this is ~90-500 rows — negligible storage.

Expose via `aimee memory health`:
```
Memory Health (last 7 days):
  Contradiction rate: 3.2% (8/250 new memories)
  Promotion rate: 12% (6/50 L1 candidates)
  Demotion rate: 2% (1/50 L2 facts)
  Staleness: 40% of L2 facts unused in 30+ days
```

**Statistical definitions:**
- **Contradiction rate**: contradictions detected / total new memories inserted,
  over a rolling 7-day window.
- **Promotion rate**: L1→L2 promotions / L1 memories eligible for promotion (use
  count >= threshold, confidence >= threshold), per maintenance cycle.
- **Demotion rate**: L2→L1 demotions / total L2 memories, per maintenance cycle.
- **Staleness**: % of L2 facts where `last_used_at` is NULL or > 30 days ago,
  measured at query time.

### 2. Contradiction Resolution Audit Log

When contradictions are detected during `memory_insert()` or
`memory_smart_insert()`, log both conflicting memories and the resolution to a
dedicated table:

```sql
CREATE TABLE contradiction_log (
    id INTEGER PRIMARY KEY,
    detected_at TEXT NOT NULL,
    memory_a_id INTEGER NOT NULL,
    memory_b_id INTEGER NOT NULL,
    resolution TEXT NOT NULL,       -- 'a_decayed', 'b_decayed', 'merged', 'manual'
    details TEXT                     -- human-readable explanation
);
```

Retention: 90 days, pruned during maintenance (same as `memory_health`).

Privacy: The audit log stores memory IDs, not content. To review, the operator
queries the memories table by ID. If a memory has been deleted, the log entry
remains but the content is gone — this is intentional.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | New migration: `memory_health` and `contradiction_log` tables |
| `src/memory_promote.c` | Write health metrics after each maintenance cycle |
| `src/memory.c` | Write to `contradiction_log` on contradiction detection |
| `src/cmd_memory.c` | `memory health` subcommand |

## Acceptance Criteria

- [ ] `aimee memory health` shows contradiction rate, promotion rate, demotion
      rate, and staleness over a 7-day window
- [ ] `memory_health` table is populated after each maintenance cycle
- [ ] Contradiction log records both IDs and resolution for every detected conflict
- [ ] Both tables are pruned to 90 days during maintenance

## Owner and Effort

- **Owner:** TBD
- **Effort:** S
- **Dependencies:** None (next migration number will be determined at implementation time)

## Rollout and Rollback

- **Rollout:** New migration creates tables on next `db_open()`. Health metrics
  start populating on next `session-start`.
- **Rollback:** Revert commit. Empty tables remain in database (harmless).
  Migration rollback not needed — tables are additive.
- **Blast radius:** New tables and queries only. No changes to existing memory
  behavior.

## Test Plan

- [ ] Unit test: `memory_run_maintenance()` writes correct counts to
      `memory_health` table
- [ ] Unit test: contradiction detection writes to `contradiction_log`
- [ ] Integration test: run 5 maintenance cycles, verify `memory health` output
      shows rolling statistics
- [ ] Integration test: contradiction log entries contain expected fields (timestamp, conflicting keys, resolution)

## Operational Impact

- **Metrics:** `memory_health` table is the metrics store (queryable via SQLite).
- **Logging:** Contradiction detection logged to stderr.
- **Alerts:** None.
- **Disk/CPU/Memory:** ~500 rows in `memory_health` (90 days), ~100 rows in
  `contradiction_log`. Negligible.

## Priority

P2 — improves trust in the memory system but not blocking for basic operation.

## Trade-offs

**Why store memory IDs in the contradiction log, not content?** Content
duplication wastes space and creates a secondary sensitive data store. IDs are
sufficient for review, and stale entries naturally become unresolvable when the
source memories are deleted — which is acceptable.

## Future Work

Confidence calibration (exploration budget, decay exemptions, confidence floor,
`aimee memory calibrate` command) is deferred to a follow-up proposal. These
changes alter memory system behavior and should be designed after health metrics
have been running long enough to establish baselines for what "healthy" looks
like.
