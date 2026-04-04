# Proposal: Adaptive Compaction

## Problem

Window compaction in `memory_context.c:9-130` applies uniform pruning regardless of memory quality:

- **raw → summary (>30 days):** Keep top 10 terms, delete the rest (~90% loss)
- **summary → fact (>90 days):** Keep top 5 terms, delete file refs (~95% loss)

This means a high-confidence, frequently-used memory about critical infrastructure topology loses the same detail as a throwaway scratch note about a one-off debugging session. The `ORDER BY term LIMIT 10` clause (`memory_context.c:36`) doesn't consider term importance — it orders alphabetically, not by relevance.

## Goals

- High-value windows retain more detail during compaction.
- Term selection considers frequency/importance, not just alphabetical order.
- Compaction aggressiveness scales with memory quality signals.

## Approach

### 1. Quality-Scaled Term Retention

Instead of fixed LIMIT 10 / LIMIT 5, scale the term count by the window's quality score:

```c
/* Quality score: based on associated memory confidence and use_count */
int base_terms = (tier == SUMMARY) ? 10 : 5;
int quality_bonus = 0;

/* Check if any L2 memory references this window */
if (has_l2_memory_for_window(db, window_id))
    quality_bonus = base_terms;  /* Double the terms kept */
else if (avg_confidence_for_window(db, window_id) > 0.8)
    quality_bonus = base_terms / 2;  /* 50% more terms */

int keep_terms = base_terms + quality_bonus;
```

This gives high-value windows 10-20 terms at summary tier (vs. fixed 10) and 5-10 at fact tier (vs. fixed 5).

### 2. Frequency-Based Term Selection

Replace alphabetical ordering with frequency ordering. The `window_terms` table already stores terms — add a `frequency` column (or use COUNT from the term extraction phase):

```sql
-- Current (alphabetical, arbitrary):
ORDER BY term LIMIT 10

-- Proposed (by importance):
ORDER BY frequency DESC, LENGTH(term) DESC LIMIT ?
```

If adding a column is too invasive, approximate by preferring longer terms (which are more specific) as a tiebreaker:

```sql
ORDER BY LENGTH(term) DESC LIMIT ?
```

### 3. Preserve File Refs for L2-Linked Windows

Currently, summary→fact compaction deletes all file refs (`DELETE FROM window_files WHERE window_id = ?` at `memory_context.c:89`). For windows linked to L2 memories, keep the top 3 file refs instead of deleting all:

```sql
DELETE FROM window_files WHERE window_id = ?
  AND rowid NOT IN (
    SELECT rowid FROM window_files WHERE window_id = ?
    ORDER BY rowid LIMIT 3
  )
```

### Changes

| File | Change |
|------|--------|
| `src/memory_context.c` | Quality-scaled term limits, frequency-based ordering, conditional file ref preservation |
| `src/headers/aimee.h` | `COMPACT_QUALITY_BONUS_L2 2.0`, `COMPACT_QUALITY_BONUS_HIGH 1.5` constants |

## Acceptance Criteria

- [ ] Windows linked to L2 memories retain 2x terms during raw→summary compaction
- [ ] Windows with avg confidence >0.8 retain 1.5x terms
- [ ] Term selection prefers longer/more-specific terms over short ones
- [ ] File refs preserved (top 3) for L2-linked windows during summary→fact
- [ ] Low-value windows compact identically to current behavior (no regression)

## Owner and Effort

- **Owner:** TBD
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Changes apply to next compaction cycle. Already-compacted windows are unaffected (data already pruned — this is not recoverable).
- **Rollback:** Revert commit. Future compaction reverts to fixed limits.
- **Blast radius:** Windows retain slightly more data → marginally higher disk usage. Bounded by 2x max terms (20 instead of 10). Negligible.

## Test Plan

- [ ] Unit test: window with L2-linked memory retains 20 terms at summary tier
- [ ] Unit test: window without L2 link retains 10 terms (unchanged)
- [ ] Unit test: term selection prefers longer terms as tiebreaker
- [ ] Unit test: file refs preserved for L2-linked windows at fact tier
- [ ] Integration test: run compaction on mixed-quality windows, verify differential retention

## Operational Impact

- **Metrics:** None new.
- **Logging:** Compaction counts already logged; add quality-tier breakdown.
- **Alerts:** None.
- **Disk/CPU/Memory:** Slightly more data retained for high-value windows. One additional query per window to check L2 linkage. Bounded to 500 windows per cycle.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Quality-scaled term retention | P2 | S | Preserves important detail |
| Frequency-based term selection | P3 | S | Better term quality |
| File ref preservation | P3 | S | Maintains artifact links |

## Trade-offs

**Why not skip compaction entirely for L2-linked windows?** Unbounded growth. Even high-value windows accumulate noise. Scaling retention (2x not infinity) preserves more signal while still bounding storage.

**Why LENGTH(term) as a proxy for importance?** Longer terms are more specific ("postgresql" vs "db", "authentication" vs "auth"). This is a crude heuristic but better than alphabetical. True frequency-based ordering requires schema changes to track term counts, which could be a follow-up.

**Why only 2x and 1.5x multipliers?** Larger multipliers make compaction ineffective for high-value windows, defeating the purpose of the tiered system. 2x is enough to preserve critical context without unbounded growth.
