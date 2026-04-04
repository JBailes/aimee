# Proposal: Cross-Workspace Memory Sharing

## Problem

Memories are stored in a single global database but context assembly (`memory_context.c:176`) makes no distinction between workspaces. This creates two problems:

1. **Noise:** When working in `wol-realm/`, memories about `aimee` internals or `infrastructure` deployment consume context budget without relevance.
2. **No cross-pollination for shared concerns:** Patterns learned in one workspace (e.g., "PostgreSQL cert auth requires specific CN format") could benefit other workspaces that share the same infrastructure, but there's no mechanism to tag or propagate shared knowledge.

The workspace-scoped memory proposal (complete) addressed the first problem partially by filtering at session start, but didn't address shared concerns or memory tagging.

## Goals

- Memories can be tagged with one or more workspace scopes.
- Context assembly prioritizes workspace-relevant memories but still includes high-confidence cross-cutting memories.
- Shared infrastructure knowledge (networking, auth, deployment) propagates to all relevant workspaces without duplication.

## Approach

### 1. Workspace Tags on Memories

Add a `memory_workspaces` junction table:

```sql
CREATE TABLE memory_workspaces (
    memory_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,
    workspace TEXT NOT NULL,
    PRIMARY KEY (memory_id, workspace)
);
CREATE INDEX idx_mw_workspace ON memory_workspaces(workspace);
```

When a memory is inserted, auto-tag it with the current workspace (from `cfg.workspaces[]` matching against cwd). Memories can have multiple workspace tags (e.g., a networking fact tagged with both `wol-realm` and `infrastructure`).

### 2. Explicit Cross-Workspace Tag

Add a reserved workspace tag `_shared` for memories that apply everywhere. Infrastructure facts, security patterns, and deployment conventions default to `_shared` when they match keywords like "network", "deploy", "auth", "postgres", "spire", "proxmox".

CLI: `aimee memory store --workspace=_shared key "content"` or `aimee memory tag <id> <workspace>`.

### 3. Context Assembly Prioritization

Modify `memory_assemble_context()` to use a two-pass approach:

**Pass 1:** Workspace-scoped memories (current workspace + `_shared`), sorted by confidence/use_count as today. Budget: 70% of `MAX_CONTEXT_TOTAL`.

**Pass 2:** High-confidence memories from other workspaces (confidence >= 0.9, use_count >= 5). Budget: remaining 30%.

Memories with no workspace tags (legacy) are treated as `_shared`.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | New migration: `memory_workspaces` table |
| `src/memory.c` | Auto-tag workspace on `memory_insert()` based on cwd |
| `src/memory_context.c` | Two-pass context assembly with workspace prioritization |
| `src/cmd_memory.c` | `--workspace` flag on `store`, new `tag` subcommand |
| `src/headers/aimee.h` | `SHARED_WORKSPACE "_shared"` constant |

## Acceptance Criteria

- [ ] `memory_insert()` auto-tags memories with the current workspace
- [ ] `aimee memory store --workspace=_shared key "content"` tags as shared
- [ ] `aimee memory tag <id> <workspace>` adds a workspace tag
- [ ] Context assembly prioritizes current-workspace and `_shared` memories
- [ ] High-confidence cross-workspace memories appear in context (pass 2)
- [ ] Legacy untagged memories are treated as `_shared`

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None (workspace-scoped memory proposal is already complete)

## Rollout and Rollback

- **Rollout:** Migration creates junction table. Existing memories get no tags (treated as `_shared`). New memories auto-tagged going forward.
- **Rollback:** Revert commit. Empty junction table remains. Context assembly falls back to current unscoped behavior.
- **Blast radius:** Context assembly changes could surface different memories than before. Low risk since legacy memories default to `_shared`.

## Test Plan

- [ ] Unit test: `memory_insert()` with cwd matching a workspace auto-tags
- [ ] Unit test: context assembly returns workspace-scoped memories first
- [ ] Unit test: high-confidence cross-workspace memories appear in pass 2
- [ ] Unit test: untagged memories are included (backward compatibility)
- [ ] Integration test: start session in `wol-realm/`, verify context excludes `aimee`-only memories
- [ ] Manual: store shared and scoped memories, verify context output

## Operational Impact

- **Metrics:** None new (existing `aimee memory stats` suffices).
- **Logging:** Workspace auto-tagging logged at debug level.
- **Alerts:** None.
- **Disk/CPU/Memory:** One row per memory per workspace in junction table. ~1-3 rows per memory. Negligible.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Workspace junction table + auto-tagging | P2 | S | Foundation |
| Two-pass context assembly | P2 | M | Reduces noise, improves relevance |
| `_shared` tag + keyword auto-detection | P3 | S | Convenience |

## Trade-offs

**Why a junction table instead of a TEXT column with comma-separated values?** Junction table enables indexed queries, proper foreign key cascades, and clean multi-workspace queries without LIKE patterns.

**Why 70/30 budget split?** The current workspace should dominate context since it's what the agent is working on. 30% for cross-workspace ensures valuable shared knowledge isn't lost. These can be tuned after the quality metrics proposal provides data on context utilization.
