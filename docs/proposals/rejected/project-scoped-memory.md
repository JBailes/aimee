# Proposal: Project-Scoped Memory

## Problem

Aimee's current memory system (L0-L3 tiers) stores facts globally or session-scoped. In multi-project workspaces, context injection via `session-start` pulls the "top 15 most used" facts globally. This often injects infrastructure details for Project A while the user is working on Project B, wasting tokens and potentially confusing the agent with irrelevant constraints.

## Goals

- Increase context injection relevance by prioritizing project-specific facts.
- Reduce token waste by excluding unrelated project memories from session starts.
- Allow users/agents to explicitly query memories for a specific project.

## Approach

Add a `project_name` column to the `memories` table. Update the retrieval logic in `cmd_hooks.c` to filter by the current project (detected via CWD) while still allowing global fallback for infrastructure/general facts.

### Schema Changes

```sql
ALTER TABLE memories ADD COLUMN project_name TEXT DEFAULT NULL;
CREATE INDEX idx_memories_project ON memories(project_name) WHERE project_name IS NOT NULL;
```

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Add migration #37 for `project_name` column and index. |
| `src/memory.c` | Update `memory_insert` and `memory_promote` to accept/preserve `project_name`. |
| `src/cmd_hooks.c` | Update `build_session_context` to filter L2 facts by current project name + global facts. |
| `src/cmd_memory.c` | Add `--project <name>` flag to `aimee memory list` and `search`. |

## Acceptance Criteria

- [ ] `aimee memory list --project foo` only shows memories where `project_name = 'foo'`.
- [ ] Session start in directory `project-a` injects facts tagged with `project-a` plus global facts (NULL project).
- [ ] No performance regression in memory retrieval (latency < 10ms).

## Owner and Effort

- **Owner:** Agent
- **Effort:** S (1-2 days)
- **Dependencies:** None

## Test Plan

- [ ] Unit tests: Verify `memory_insert` correctly tags project name.
- [ ] Integration tests: Run session-start in two different project dirs and verify different context outputs.
- [ ] Manual verification: Use `aimee memory list` to confirm scoping.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Schema Update | P1 | S | Low |
| Filtered Retrieval | P1 | S | High (Token savings) |
| CLI Flags | P2 | S | Medium |
