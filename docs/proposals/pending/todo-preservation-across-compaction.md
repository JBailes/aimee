# Proposal: Todo Preservation Across Compaction

## Problem

When context compaction occurs mid-session, the in-memory task/todo list can be lost. The agent forgets what it was working on, which tasks are complete, and what remains. This causes duplicate work, missed tasks, and confused delegates that restart from scratch.

Evidence: oh-my-openagent implements a `compaction-todo-preserver` hook (`src/hooks/compaction-todo-preserver/hook.ts`) that snapshots the todo list before compaction and restores it after. This ensures task state survives compaction intact.

## Goals

- Snapshot task state before compaction occurs
- Restore task state after compaction completes
- Task status (pending/in_progress/completed) is preserved exactly
- Works for both orchestrator and delegate sessions

## Approach

Before compaction, serialize the current task list to a temporary store (in-memory map keyed by session ID). After compaction completes, re-inject the task list into the session state.

### Changes

| File | Change |
|------|--------|
| `src/tasks.c` | Add `tasks_snapshot()` and `tasks_restore()` functions |
| `src/agent_context.c` | Call snapshot before compaction, restore after |

## Acceptance Criteria

- [ ] Task list is identical before and after compaction
- [ ] Task status (pending/in_progress/completed) is preserved
- [ ] Snapshot is scoped per session
- [ ] Snapshot is cleaned up when session ends
- [ ] Works when compaction occurs during an in_progress task

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (< 1 day)
- **Dependencies:** Task system and compaction both implemented

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; automatic
- **Rollback:** Remove snapshot/restore; tasks may be lost on compaction as before
- **Blast radius:** Only affects task state during compaction events

## Test Plan

- [ ] Unit test: snapshot captures all tasks with correct status
- [ ] Unit test: restore recreates tasks identically
- [ ] Unit test: snapshot for session A doesn't affect session B
- [ ] Integration test: compaction mid-session, verify tasks survive

## Operational Impact

- **Metrics:** Compaction events with task preservation count
- **Logging:** Log snapshot/restore at debug level
- **Disk/CPU/Memory:** One task list copy per session during compaction; negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Todo Preservation | P2 | S | Medium — prevents task loss during compaction |

## Trade-offs

Alternative: persist tasks to disk continuously (proposal #15). More durable but higher overhead. In-memory snapshot is simpler and covers the compaction case specifically.

Inspiration: oh-my-openagent `src/hooks/compaction-todo-preserver/hook.ts`
