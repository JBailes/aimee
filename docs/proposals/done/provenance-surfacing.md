# Proposal: Provenance Surfacing

## Problem

The `memory_provenance` table tracks a rich audit trail — insert, merge, supersede actions with timestamps and session IDs — but this data is completely inaccessible. There is no CLI command to query it, no integration with context assembly, and no way for an agent or operator to ask "why do I know this?" or "when did this memory last change?"

This matters because memories decay, get merged, and get superseded silently. An agent acting on a stale memory has no way to evaluate its freshness or lineage. The provenance data exists (`memory_advanced.c:339-358`) and is written correctly, but is write-only.

## Goals

- Operators can inspect the full history of any memory (creation, merges, supersessions).
- Agents can see provenance metadata when a memory is surfaced in context.
- Stale or frequently-superseded memories are flagged for review.

## Approach

### 1. CLI: `aimee memory provenance <id>`

Display the full provenance chain for a memory:

```
Memory #42: wol_network_topology (L2, fact, confidence: 0.95)
  2026-01-15  insert    session:abc123  "Initial creation"
  2026-02-03  merge     session:def456  "Key exact match, content updated"
  2026-03-10  supersede session:ghi789  "supersedes memory 38"
```

Implementation: query `memory_provenance WHERE memory_id = ? ORDER BY created_at ASC`, format as table.

### 2. CLI: `aimee memory provenance --stale`

List memories with suspicious provenance patterns:

- **Never used:** L2 memories where `use_count = 0` and age > 14 days
- **Frequently superseded:** Memories whose key has 3+ versions (from `memory_supersede()` versioning)
- **Orphaned provenance:** Provenance entries referencing deleted memories

```sql
-- Frequently superseded keys
SELECT SUBSTR(key, 1, INSTR(key, '#v') - 1) AS base_key, COUNT(*) AS versions
FROM memories
WHERE key LIKE '%#v%'
GROUP BY base_key
HAVING versions >= 3
```

### 3. Provenance Hint in Context Assembly

When a memory is included in assembled context and has been superseded (has provenance action='supersede'), append a freshness marker:

```
- wol_network_topology: WOL uses two isolated bridges... (updated 2026-03-10)
```

This uses ~20 chars per flagged memory. Only apply to memories with supersede provenance, not all memories.

### Changes

| File | Change |
|------|--------|
| `src/cmd_memory.c` | `provenance` subcommand with `--stale` flag |
| `src/memory_context.c` | Append freshness marker for superseded memories in context output |
| `src/headers/aimee.h` | Declare `memory_get_provenance()` |
| `src/memory.c` or `src/memory_advanced.c` | `memory_get_provenance()` query function |

## Acceptance Criteria

- [ ] `aimee memory provenance <id>` shows full provenance chain with timestamps and actions
- [ ] `aimee memory provenance --stale` lists never-used L2s, frequently superseded keys, and orphaned provenance
- [ ] Context assembly appends "(updated YYYY-MM-DD)" to superseded memories
- [ ] Provenance for deleted memories returns "memory not found" gracefully

## Owner and Effort

- **Owner:** TBD
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** New CLI subcommand and minor context assembly change. No migration needed — `memory_provenance` table already exists.
- **Rollback:** Revert commit. Provenance data continues to be written (existing behavior) but is again inaccessible.
- **Blast radius:** Context assembly change adds a few characters per superseded memory. Minimal impact on context budget.

## Test Plan

- [ ] Unit test: `memory_get_provenance()` returns correct chain after insert, merge, supersede
- [ ] Unit test: `--stale` identifies never-used L2 memories
- [ ] Unit test: `--stale` identifies keys with 3+ versions
- [ ] Integration test: create, supersede, query provenance end-to-end
- [ ] Manual: verify context output includes freshness markers

## Operational Impact

- **Metrics:** None new.
- **Logging:** None new.
- **Alerts:** None.
- **Disk/CPU/Memory:** Read-only queries against existing table. Negligible.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| `provenance` CLI command | P2 | S | Enables debugging and trust |
| `--stale` flag | P2 | S | Identifies memory hygiene issues |
| Context freshness markers | P3 | S | Subtle but useful |

## Trade-offs

**Why not show provenance for every memory in context?** Most memories have a single "insert" provenance entry. Adding "(created 2026-01-15)" to every memory wastes context budget without adding value. Only superseded memories benefit from freshness markers, because their content has changed over time.

**Why not auto-delete stale memories?** Staleness detection is a signal, not an action. Some memories are correctly high-confidence but rarely used (e.g., security constraints). Deletion should be a deliberate operator action, informed by the stale report.
