# Proposal: Session Resume, Handoff, and Persistent Plan Progress

## Problem

The pending set has three continuity proposals that all address the same failure: work is lost when a session or plan is interrupted.

- chat/webchat sessions cannot be resumed cleanly
- users cannot export a compact handoff artifact to continue elsewhere
- plan progress is too ephemeral and can disappear on crash/restart

These should be one continuity proposal covering automatic resume, explicit handoff, and persisted plan state.

## Goals

- Users can resume prior chat/webchat sessions.
- Sessions can be exported as a structured handoff document for clean continuation in a new session.
- Plan execution progress survives crashes, restarts, and handoffs.
- Continuity data is queryable and searchable rather than living only in memory.

## Approach

Implement one continuity subsystem with three surfaces:

1. session transcript persistence and resume
2. explicit handoff export
3. persisted plan progress state

### Session Storage and Resume

Persist chat messages, tool state, and prompt context to a searchable store. Support:

```bash
aimee chat --resume
aimee chat --resume <session-id>
aimee chat --list-sessions
```

Webchat should expose a recent-session picker and resume endpoints.

### Handoff Export

Add `aimee handoff` to emit a compact, self-contained continuation document including:

- original request
- completed work
- remaining tasks
- key decisions
- modified files
- constraints
- next recommended action

### Persistent Plan Progress

Persist active plan state to `.aimee/plan-state.json` or an equivalent resumable store so incomplete plans can be detected and resumed after interruption.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Add `session_messages` table and continuity metadata |
| `src/server_session.c` | Message persistence, session search, resume handler |
| `src/cmd_chat.c` | `--resume`, `--list-sessions`, and handoff hooks |
| `src/webchat.c` | Session list and resume endpoints |
| `src/webchat_assets.c` | Session picker sidebar |
| `src/tasks.c` | Persistent plan progress serialization |
| `src/agent_plan.c` | Detect incomplete plan state and offer resume |

## Acceptance Criteria

- [ ] `aimee chat --list-sessions` shows recent sessions with title, date, and message count.
- [ ] `aimee chat --resume` resumes the most recent session with full history.
- [ ] Webchat session picker shows recent sessions and allows one-click resume.
- [ ] `aimee handoff` exports a structured continuation document with required sections.
- [ ] Incomplete plans are detected after restart and can be resumed with preserved task status.
- [ ] Sessions remain searchable by title substring.

## Owner and Effort

- **Owner:** aimee
- **Effort:** L
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Start with transcript persistence and resume, then add handoff export, then persist plan progress.
- **Rollback:** Each surface can be removed independently; older continuity data can simply be ignored.
- **Blast radius:** Mainly chat, webchat, and plan recovery paths.

## Test Plan

- [ ] Unit tests: message persist, load, search
- [ ] Unit tests: handoff document includes required sections
- [ ] Unit tests: plan state persistence and resume detection
- [ ] Integration tests: start session → exit → resume → verify history intact
- [ ] Integration tests: interrupt a plan mid-execution, restart, verify resume

## Operational Impact

- **Metrics:** `session_resumed`, `session_messages_stored`, `handoff_exports`, `plan_resume_detected`
- **Logging:** INFO on session resume/persist/handoff generation, WARN on corrupted continuity state
- **Alerts:** None
- **Disk/CPU/Memory:** Moderate disk for transcripts and lightweight plan-state files

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Session resume | P1 | M | High |
| Handoff export | P2 | S | Medium |
| Persistent plan progress | P1 | S | High |

## Trade-offs

- **Why merge these continuity proposals?** Resume, handoff, and persisted plan progress solve the same interruption boundary at different levels.
- **Why keep both resume and handoff?** Resume is automatic continuation of the same state; handoff is a portable summary for a fresh state.
- **Known limitation:** Very large sessions may need compaction-aware resume rather than raw replay.
