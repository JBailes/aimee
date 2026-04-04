# Proposal: Session Resume for Chat and Webchat

## Problem

When a chat session ends (user exits, connection drops, timeout), the conversation is gone. The user must start fresh, re-explain context, and redo work. Aimee's server tracks sessions in `server_sessions` and the CLI can `--resume` Claude sessions, but there is no first-class session resume for aimee's own chat and webchat modes.

Mistral-vibe implements session resume with `SessionLoader` (list/search/load past sessions), `resume_sessions.py` (unified local+remote resume), and message persistence in JSONL format. Users can list recent sessions and continue any of them.

## Goals

- Users can list recent chat sessions and resume any of them (CLI and webchat).
- Session messages, tool state, and system prompt are restored on resume.
- Webchat shows a session picker sidebar for quick resume.
- Sessions are searchable by title, date, and working directory.

## Approach

### Storage

Persist conversation messages to the session DB alongside existing `server_sessions` metadata. Each message is stored as a row in a new `session_messages` table with role, content, tool call data, and sequence number. The first user message becomes the session title.

### CLI

```
aimee chat --resume              # resume most recent session
aimee chat --resume <session-id> # resume specific session
aimee chat --list-sessions       # list recent sessions
```

On resume, load messages from the DB, rebuild the conversation array, and continue the agent loop from where it left off.

### Webchat

- Session list endpoint: `GET /api/sessions` returns recent sessions with title, timestamp, message count.
- Resume endpoint: `POST /api/sessions/<id>/resume` loads messages and starts SSE streaming.
- Session picker sidebar in the webchat UI (already has tabs — add session history to the sidebar).

### Message Persistence

After each agent turn, append new messages to the `session_messages` table. This is incremental — only new messages since last persist are written. Use a sequence counter to maintain order.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Add `session_messages` table (session_id, seq, role, content, tool_calls, tool_results, created_at) |
| `src/server_session.c` | Add message persistence, session search, resume handler |
| `src/cmd_chat.c` | Add `--resume`, `--list-sessions` flags; load messages on resume |
| `src/webchat.c` | Add session list and resume API endpoints |
| `src/webchat_assets.c` | Add session picker sidebar to webchat UI |
| `src/headers/server.h` | New handler declarations |

## Acceptance Criteria

- [ ] `aimee chat --list-sessions` shows recent sessions with title, date, message count
- [ ] `aimee chat --resume` resumes the most recent session with full message history
- [ ] `aimee chat --resume <id>` resumes a specific session
- [ ] Webchat session picker shows recent sessions and allows one-click resume
- [ ] Resumed session preserves tool call history and system prompt
- [ ] Session title auto-derived from first user message (max 80 chars)
- [ ] Sessions searchable by title substring

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2-3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** New DB table auto-created. CLI flags are additive. Webchat sidebar is new UI.
- **Rollback:** Drop table. Remove flags. No impact on existing sessions.
- **Blast radius:** Only affects chat/webchat. No impact on delegates or server operations.

## Test Plan

- [ ] Unit tests: message persist, load, search
- [ ] Integration tests: start session → exit → resume → verify history intact
- [ ] Failure injection: resume with corrupted messages — verify graceful degradation
- [ ] Manual verification: webchat session picker flow end-to-end

## Operational Impact

- **Metrics:** `session_resumed` counter, `session_messages_stored` gauge
- **Logging:** INFO on session resume/persist, WARN on load failures
- **Alerts:** None
- **Disk/CPU/Memory:** Moderate disk for message storage. Add periodic cleanup of sessions older than configurable retention (default 30 days).

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Session Resume | P2 | M | High — major UX improvement, eliminates wasted re-explanation |

## Trade-offs

**Alternative: Store messages as JSONL files.** Simpler but harder to search and query. SQLite is already the persistence layer and supports indexing.

**Alternative: Only store the last N sessions.** Implemented as retention policy — default 30 days, configurable.

**Known limitation:** Large sessions (hundreds of turns) may be slow to reload. Mitigated by only loading the last N messages and summarizing earlier ones (pairs with session-compaction proposal).
