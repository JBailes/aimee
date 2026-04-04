# Proposal: Retroactive Conflict Detection

## Problem

Conflict detection in `memory_promote.c:335-358` only runs at insertion time — when `memory_detect_conflict()` is called from `memory_insert()` or `memory_smart_insert()`. This means contradictions that develop over time go undetected:

1. **Supersession drift:** Memory A says "WOL uses vmbr1 for prod." Memory B, inserted weeks later, says "WOL uses vmbr4 for prod." If B has a different key than A, the conflict is never detected because `memory_detect_conflict()` requires exact key match.
2. **Cross-kind conflicts:** A fact and a decision may contradict each other (fact: "always use mTLS" vs decision: "skipped mTLS for internal health checks"). These are never compared because conflicts only check within the same key.
3. **Accumulated contradictions:** Multiple incremental merges can drift content until it contradicts other memories that were consistent at the time of their respective insertions.

Evidence: `memory_detect_conflict()` at `memory_promote.c:335` only queries `WHERE key = ?`. The `is_contradiction()` function in `text.c` can detect negation-based contradictions but is never called across different keys.

## Goals

- Periodic retroactive scans detect contradictions across different keys.
- Cross-kind contradictions (fact vs decision) are detected.
- Detected conflicts are recorded in `memory_conflicts` for operator review.

## Approach

### 1. Periodic Cross-Key Scan

Add `memory_scan_retroactive_conflicts()` called from `memory_run_maintenance()`. Strategy: compare L2 memories pairwise within the same semantic cluster.

**Clustering by terms:** Group L2 memories that share 2+ significant terms (from FTS5 or content tokenization). Only compare within clusters to avoid O(n^2) on all memories.

```sql
-- Find candidate pairs sharing content terms
WITH mem_terms AS (
    SELECT m.id, m.key, m.content,
           LOWER(m.key || ' ' || m.content) AS text
    FROM memories m
    WHERE m.tier = 'L2' AND m.confidence > 0.5
)
SELECT a.id AS id_a, b.id AS id_b, a.content, b.content
FROM mem_terms a, mem_terms b
WHERE a.id < b.id
  AND a.key != b.key
  /* At least some term overlap (coarse filter) */
  AND (
    a.text LIKE '%' || SUBSTR(b.key, 1, INSTR(b.key || '_', '_') - 1) || '%'
    OR b.text LIKE '%' || SUBSTR(a.key, 1, INSTR(a.key || '_', '_') - 1) || '%'
  )
LIMIT 200
```

For each candidate pair, call `is_contradiction()`. If positive, record in `memory_conflicts`.

### 2. Cross-Kind Comparison

Extend the scan to compare facts against decisions:

```sql
-- Facts that mention terms from decisions and vice versa
SELECT f.id, d.id, f.content, d.content
FROM memories f, memories d
WHERE f.kind = 'fact' AND d.kind = 'decision'
  AND f.tier IN ('L1', 'L2') AND d.tier IN ('L2', 'L3')
  AND f.id != d.id
  AND (f.content LIKE '%' || d.key || '%' OR d.content LIKE '%' || f.key || '%')
LIMIT 100
```

### 3. Rate Limiting

Retroactive scanning is expensive. Limit to:
- Run at most once per day (track last scan timestamp in `memory_health` or a config key)
- Cap at 200 candidate pairs per scan
- Skip if total L2 memory count < 10 (not enough data to have meaningful conflicts)

### Changes

| File | Change |
|------|--------|
| `src/memory_promote.c` | Add `memory_scan_retroactive_conflicts()`, call from `memory_run_maintenance()` with daily rate limit |
| `src/headers/aimee.h` | `RETRO_CONFLICT_INTERVAL 86400` (seconds), `RETRO_CONFLICT_MAX_PAIRS 200` |

## Acceptance Criteria

- [ ] `memory_run_maintenance()` runs retroactive conflict scan at most once per day
- [ ] Cross-key contradictions between L2 memories are detected and recorded
- [ ] Cross-kind contradictions (fact vs decision) are detected
- [ ] `aimee memory conflicts` shows retroactively detected conflicts
- [ ] Scan completes in <500ms with 100+ L2 memories

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None (uses existing `memory_conflicts` table and `is_contradiction()`)

## Rollout and Rollback

- **Rollout:** New logic in maintenance cycle. First scan runs on next maintenance after deploy.
- **Rollback:** Revert commit. Existing conflicts remain in table. Insertion-time detection continues.
- **Blast radius:** Read-only scan + writes to `memory_conflicts`. No changes to existing memories.

## Test Plan

- [ ] Unit test: two L2 memories with contradictory content and different keys detected
- [ ] Unit test: non-contradictory memories with shared terms not flagged
- [ ] Unit test: cross-kind contradiction (fact vs decision) detected
- [ ] Unit test: rate limiting prevents scan from running twice in 24h
- [ ] Unit test: scan skipped when L2 count < 10
- [ ] Benchmark: 200 candidate pairs scanned in <500ms

## Operational Impact

- **Metrics:** Retroactive conflicts detected per scan (in `memory_health` if that proposal lands).
- **Logging:** Scan start/end and conflict count logged to stderr.
- **Alerts:** None.
- **Disk/CPU/Memory:** 200 pair comparisons × `is_contradiction()` (string ops). ~50-200ms per scan. Daily only.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Cross-key conflict scan | P2 | M | Catches hidden contradictions |
| Cross-kind comparison | P3 | S | Broader coverage |
| Rate limiting | P2 | S | Prevents performance impact |

## Trade-offs

**Why cluster-then-compare instead of all-pairs?** With 100 L2 memories, all-pairs is 4,950 comparisons. Clustering by shared terms reduces this to ~200 candidate pairs. The coarse LIKE filter misses some conflicts but avoids quadratic cost.

**Why daily, not per-session?** Retroactive conflicts develop slowly (days/weeks). Per-session scanning wastes cycles checking the same pairs repeatedly. Daily is sufficient to catch drift within a reasonable window.

**Why not auto-resolve?** Retroactive conflicts are subtle — they involve different keys with overlapping semantics. Auto-resolution (pick higher confidence) could silently discard correct information. These should surface for operator review.
