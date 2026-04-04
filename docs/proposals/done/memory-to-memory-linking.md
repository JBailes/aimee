# Proposal: Memory-to-Memory Linking

## Problem

The entity graph (`memory_graph.c`) connects entities (files, terms) via co-editing and co-discussion relationships, but there is no way to express direct relationships between memories themselves:

1. **"Memory A supersedes memory B"** — tracked in provenance but not queryable as a graph edge. `memory_supersede()` (`memory_advanced.c:360`) renames the old key to `key#vN` but doesn't create a navigable link.
2. **"Memory A depends on memory B"** — if a deployment fact depends on a network topology fact, invalidating the topology should flag the deployment fact. No mechanism for this.
3. **"Memory A contradicts memory B"** — recorded in `memory_conflicts` but not connected to the entity graph. Conflict resolution doesn't propagate to related memories.

The `entity_edges` table has `source`, `relation`, `target` as TEXT fields representing entity names, not memory IDs. Memories are identified by key, which changes on supersession (`key#vN`).

## Goals

- Memories can be explicitly linked with typed relationships (supersedes, depends_on, contradicts, related_to).
- Invalidating or demoting a memory cascades warnings to dependent memories.
- Links are navigable via CLI and considered during context assembly.

## Approach

### 1. Memory Links Table

```sql
CREATE TABLE memory_links (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,
    target_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,
    relation TEXT NOT NULL,  -- 'supersedes', 'depends_on', 'contradicts', 'related_to'
    created_at TEXT NOT NULL,
    UNIQUE(source_id, target_id, relation)
);
CREATE INDEX idx_ml_source ON memory_links(source_id);
CREATE INDEX idx_ml_target ON memory_links(target_id);
```

### 2. Auto-Link on Supersession

Modify `memory_supersede()` to create a `supersedes` link from the new memory to the old:

```c
/* After inserting new memory and renaming old key */
memory_link_create(db, new_mem.id, old_id, "supersedes");
```

### 3. Auto-Link on Conflict Detection

Modify `memory_record_conflict()` to create a `contradicts` link:

```c
/* After recording conflict */
memory_link_create(db, mem_a, mem_b, "contradicts");
```

### 4. Manual Linking via CLI

```
aimee memory link <source_id> <target_id> --relation depends_on
aimee memory links <id>           # show all links for a memory
aimee memory unlink <link_id>     # remove a link
```

### 5. Cascade Warnings on Demotion

When `memory_demote()` demotes a memory from L2→L1, check for `depends_on` links where the demoted memory is the target:

```sql
SELECT source_id FROM memory_links
WHERE target_id = ? AND relation = 'depends_on'
```

For each dependent memory, reduce confidence by 10% and log a warning. This is a soft cascade — it flags dependents without deleting them.

### 6. Context Assembly Integration

When assembling context, if a memory has `depends_on` links to memories not in the current context, include a hint:

```
- deploy_procedure: Run deploy.sh on provisioning host (depends on: wol_network_topology)
```

This costs ~30 chars per linked memory. Only apply to memories with `depends_on` links.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | New migration: `memory_links` table |
| `src/memory_advanced.c` | `memory_link_create()`, `memory_link_query()`, `memory_link_delete()` |
| `src/memory_advanced.c` | Auto-link in `memory_supersede()` |
| `src/memory_promote.c` | Auto-link in `memory_record_conflict()`, cascade in `memory_demote()` |
| `src/memory_context.c` | Dependency hints in context output |
| `src/cmd_memory.c` | `link`, `links`, `unlink` subcommands |

## Acceptance Criteria

- [ ] `aimee memory link <src> <tgt> --relation depends_on` creates a link
- [ ] `aimee memory links <id>` shows all links (incoming and outgoing)
- [ ] `memory_supersede()` auto-creates a `supersedes` link
- [ ] `memory_record_conflict()` auto-creates a `contradicts` link
- [ ] Demoting an L2 memory cascades 10% confidence reduction to `depends_on` sources
- [ ] Context assembly includes dependency hints for linked memories
- [ ] Deleting a memory cascades link deletion (ON DELETE CASCADE)

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Migration creates table. Auto-linking starts on next supersession/conflict. Existing supersessions and conflicts are not retroactively linked (data not available).
- **Rollback:** Revert commit. Empty links table remains. Supersession and conflict detection revert to current behavior.
- **Blast radius:** Cascade demotion reduces confidence of dependent memories. This is a behavior change but soft (10% reduction, not deletion).

## Test Plan

- [ ] Unit test: `memory_link_create()` inserts link, query returns it
- [ ] Unit test: duplicate link (same source, target, relation) rejected by UNIQUE
- [ ] Unit test: `memory_supersede()` creates `supersedes` link
- [ ] Unit test: `memory_record_conflict()` creates `contradicts` link
- [ ] Unit test: demoting memory cascades confidence reduction to dependents
- [ ] Unit test: deleting memory cascades link deletion
- [ ] Integration test: create dependency chain A→B→C, demote C, verify A and B flagged
- [ ] Manual: `aimee memory links` shows readable output

## Operational Impact

- **Metrics:** Link count by relation type (queryable via `memory_links`).
- **Logging:** Auto-link creation logged at debug. Cascade demotion logged as info.
- **Alerts:** None.
- **Disk/CPU/Memory:** ~1-3 links per memory. One additional query per demotion to check dependents. Negligible.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Links table + CRUD | P2 | S | Foundation |
| Auto-link on supersede/conflict | P2 | S | Automatic graph building |
| Cascade demotion | P2 | S | Prevents stale dependents |
| Context hints | P3 | S | Subtle UX improvement |
| Manual linking CLI | P3 | S | Operator tool |

## Trade-offs

**Why a separate table instead of extending `entity_edges`?** `entity_edges` connects entity names (strings) for search boosting. `memory_links` connects memory IDs for lifecycle management (cascade, provenance). They serve different purposes and mixing them would complicate both.

**Why soft cascade (10% confidence reduction) instead of hard cascade (demotion)?** Hard cascade could trigger chain reactions — demoting one memory could cascade through a dependency chain and demote many. Soft cascade flags dependents for review without aggressive action. If confidence drops below `DEMOTE_L2_CONFIDENCE` (0.7), the normal maintenance cycle handles demotion.

**Why not retroactively link existing supersessions?** The superseded memories have been renamed to `key#vN` and are identifiable, but associating them with their successors requires matching the base key — which is already done by `memory_fact_history()`. Retroactive linking could be a follow-up if there's demand.
