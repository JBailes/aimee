# Proposal: Persistent Plan Progress Tracking

## Problem

Plan execution progress is tracked in-memory per session. If a delegate crashes, the server restarts, or a session is interrupted, all progress tracking is lost. The orchestrator cannot determine which tasks in a plan have been completed and must either restart from scratch or ask the user.

Evidence: oh-my-openagent implements "Boulder State" (`src/features/boulder-state/`) — a JSON file (`.sisyphus/boulder.json`) that persists active plan name, task completion status, session-to-task mappings, and overall progress. This enables seamless continuation after interruptions.

## Goals

- Persist plan execution progress to disk
- Track per-task completion status, assigned delegate session ID, and timestamps
- Enable plan resumption after server restart or session crash
- Automatically detect and resume incomplete plans

## Approach

Write plan progress to `.aimee/plan-state.json` on every task status change. On session start, check for incomplete plans and offer to resume.

### State file format

```json
{
  "plan": "docs/proposals/pending/foo.md",
  "started_at": "2026-04-04T10:00:00Z",
  "tasks": {
    "1": {"status": "completed", "delegate_session": "abc123", "completed_at": "..."},
    "2": {"status": "in_progress", "delegate_session": "def456", "started_at": "..."},
    "3": {"status": "pending"}
  }
}
```

### Changes

| File | Change |
|------|--------|
| `src/tasks.c` | Write task status changes to `.aimee/plan-state.json` |
| `src/agent_plan.c` | On plan start, check for existing plan-state and offer resume |
| `src/headers/tasks.h` | Add persistent state structures |

## Acceptance Criteria

- [ ] Task status changes are written to disk within 1s
- [ ] Plan state file is valid JSON and human-readable
- [ ] Server restart + session start detects incomplete plan and offers resume
- [ ] Completed plans are archived (moved to `.aimee/plan-history/`)
- [ ] Concurrent access is safe (file locking or atomic writes)

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2–3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; plan-state written automatically
- **Rollback:** Remove state writes; plans are ephemeral as before
- **Blast radius:** Adds a file to `.aimee/`; no effect on plan execution itself

## Test Plan

- [ ] Unit test: task completion writes to state file
- [ ] Unit test: state file is valid JSON after each write
- [ ] Unit test: resume detects incomplete plan correctly
- [ ] Unit test: completed plan is archived
- [ ] Integration test: interrupt a plan mid-execution, restart, verify resume

## Operational Impact

- **Metrics:** Plan completion rate, average tasks per plan, resume frequency
- **Logging:** Log state writes at debug, resume detection at info
- **Disk/CPU/Memory:** One JSON file per active plan; <1KB typical

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Persistent Plan Progress | P2 | M | Medium — enables plan recovery after crashes |

## Trade-offs

Alternative: store plan state in the database instead of a JSON file. More robust for concurrent access but adds DB schema dependency. JSON file is simpler and human-debuggable.

Inspiration: oh-my-openagent `src/features/boulder-state/`
