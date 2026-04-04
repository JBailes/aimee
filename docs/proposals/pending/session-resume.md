# Proposal: Session Persistence and Resumption

## Problem

Aimee tracks session metadata (start time, mode, workspace, provider) in `server_session.c` and persists some state to the database, but there is no mechanism to resume a previous conversation. When a delegate session ends — whether by completion, crash, or context exhaustion — the conversation history is gone. The user cannot say "resume session X" to pick up where they left off.

The claw-code project implements session persistence in `runtime/session.rs`:
- Full conversation history (messages, roles, content blocks, token usage) serialized to JSON
- `save_to_path()` / `load_from_path()` for filesystem persistence
- Session version tracking for format migrations
- A continuation strategy that inserts a system message explaining the gap when resuming

Aimee's `session-compaction` pending proposal addresses context window overflow but not persistence/resumption. The `context-compaction-awareness` pending proposal addresses awareness of compaction but not the ability to resume from a saved state.

## Goals

- Delegate sessions can be resumed after crash, timeout, or context exhaustion.
- The conversation history (messages + tool results) is persisted to disk after each turn.
- `aimee agent resume <session-id>` restarts a session from its last saved state.
- Resumption includes a system message summarizing what was accomplished and what remains.
- Old session files are garbage-collected after a configurable retention period.

## Approach

### 1. Session File Format

Each session gets a JSON file at `~/.config/aimee/sessions/<session-id>.json`:

```json
{
  "version": 1,
  "session_id": "abc123",
  "provider": "claude",
  "model": "claude-sonnet-4-6",
  "workspace": "/root/dev/aimee",
  "messages": [
    {"role": "system", "content": "..."},
    {"role": "user", "content": "..."},
    {"role": "assistant", "content": "...", "usage": {"input": 1200, "output": 450}}
  ],
  "tasks": [
    {"id": 1, "description": "...", "status": "completed"},
    {"id": 2, "description": "...", "status": "in_progress"}
  ],
  "created_at": "2026-04-04T10:00:00Z",
  "updated_at": "2026-04-04T10:05:00Z"
}
```

### 2. Persistence Hook

After each assistant turn completes (in `server_session.c`), append the new messages to the session file. Use atomic write (write to `.tmp`, rename) to prevent corruption on crash.

### 3. Resumption Flow

`aimee agent resume <session-id>`:
1. Load session file
2. If message count exceeds context budget, apply compaction to older messages (reuse `memory_context.c` summarization)
3. Prepend a system message: "This session is being resumed. Previously completed: [task summary]. Remaining: [task summary]. Continue from where you left off."
4. Start a new agent session with the loaded history

### 4. Garbage Collection

A daily cleanup (or on `aimee init`) deletes session files older than `session.retention_days` (default: 7).

### Changes

| File | Change |
|------|--------|
| `src/server_session.c` | Add `session_save_history()` called after each turn; add `session_load_history()` |
| `src/cmd_agent.c` | Add `resume` subcommand that loads and restarts a session |
| `src/config.c` | Add `session.persist` (bool) and `session.retention_days` (int) config keys |
| `src/headers/server_session.h` | Declare persistence/load API |

## Acceptance Criteria

- [ ] After a delegate completes 3 turns, `~/.config/aimee/sessions/<id>.json` exists with 3+ messages
- [ ] `aimee agent resume <id>` starts a new session with the prior conversation loaded
- [ ] Resumed session includes a system message summarizing prior progress
- [ ] Session file survives server restart (persisted to filesystem, not just memory)
- [ ] Files older than `session.retention_days` are cleaned up on next `aimee init`
- [ ] Crash mid-turn does not corrupt the session file (atomic write)

## Owner and Effort

- **Owner:** aimee maintainer
- **Effort:** M (serialization is straightforward; main complexity is compaction on resume and atomic writes)
- **Dependencies:** None (complements but does not depend on session-compaction or context-compaction-awareness proposals)

## Rollout and Rollback

- **Rollout:** `session.persist` defaults to `true`. Resume is opt-in via explicit `aimee agent resume`.
- **Rollback:** `aimee config set session.persist false` stops writing session files. Existing files can be deleted manually.
- **Blast radius:** Adds disk writes per turn (~1-10KB per turn). No impact on session execution itself.

## Test Plan

- [ ] Unit tests: `session_save_history()` / `session_load_history()` round-trip
- [ ] Integration tests: delegate crash → resume → verify continuation
- [ ] Failure injection: kill aimee-server mid-write, verify session file is not corrupted
- [ ] Manual verification: run a multi-turn delegate, resume it, confirm it picks up correctly

## Operational Impact

- **Metrics:** `session.files_written`, `session.files_cleaned` counters
- **Logging:** INFO on save/load, WARN on cleanup, ERROR on corruption
- **Alerts:** None
- **Disk/CPU/Memory:** ~1-50KB per session file, cleaned after retention period. Negligible CPU for JSON serialization.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Session persistence | P2 | M | High — enables crash recovery and long-running task continuation |

## Trade-offs

- **SQLite storage** instead of JSON files was considered. JSON files are simpler, human-readable, and don't require schema migrations. SQLite would be better at scale (thousands of sessions) but aimee's session volume doesn't warrant it.
- **Full conversation replay** vs **summary + recent messages** on resume. Full replay is more accurate but may exceed context windows for long sessions. The hybrid approach (compact old, keep recent) balances accuracy and feasibility.
- **Automatic resume on crash** was considered but is risky — if the crash was caused by the conversation content itself, auto-resume could loop. Explicit `aimee agent resume` is safer.
