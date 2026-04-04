# Proposal: Session-Based SSE API for Webchat

## Problem

Aimee's webchat (`webchat.c`) uses a request-response model for chat: the browser sends a message via `POST /api/chat/send`, the server streams SSE events back for that single turn, and the connection closes. There is no persistent session API — clients cannot:
- List active sessions or create new ones via API
- Subscribe to a session's event stream independently of sending messages
- View session state (message history, token usage) without being in an active SSE connection
- Have multiple clients observe the same session (e.g., dashboard watching a delegate's progress)

The claw-code project implements a clean session-based HTTP API in `server/src/lib.rs`:
- `POST /sessions` — create a new session, returns `{session_id}`
- `GET /sessions` — list all sessions with metadata (creation time, message count)
- `GET /sessions/{id}` — full session state including conversation history
- `POST /sessions/{id}/message` — send a message (triggers LLM turn)
- `GET /sessions/{id}/events` — SSE stream with keepalive, broadcasts `snapshot` and `message` events

This design decouples session lifecycle from individual HTTP requests and enables multi-client observation.

## Goals

- Webchat sessions have explicit lifecycle: create, query, send message, observe events, close.
- The SSE event stream is a persistent subscription, not tied to a single send request.
- Multiple clients can subscribe to the same session's events (dashboard, webchat UI, CLI).
- Session state is queryable via REST (history, usage, status) without needing SSE.
- The existing webchat UI migrates to use this API with minimal changes.

## Approach

### 1. New API Endpoints

Add to `webchat.c`'s route handler:

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/sessions` | Create session, returns `{session_id}` |
| `GET` | `/api/sessions` | List active sessions with metadata |
| `GET` | `/api/sessions/{id}` | Get session state (messages, usage, model) |
| `POST` | `/api/sessions/{id}/message` | Send user message, triggers LLM turn |
| `GET` | `/api/sessions/{id}/events` | SSE subscription with keepalive |
| `DELETE` | `/api/sessions/{id}` | Close and clean up session |

### 2. SSE Event Stream

The `/events` endpoint sends:
- **`snapshot`** event on connect: full current session state
- **`text_delta`** events: streaming text tokens
- **`tool_start`** / **`tool_result`** events: tool execution lifecycle
- **`usage`** events: per-turn token counts
- **`keepalive`** events: every 15 seconds to prevent proxy timeouts

Use a per-session broadcast pattern: the LLM turn loop writes events to a shared buffer, and all connected SSE clients read from it.

### 3. Multi-Client Support

Each session maintains a list of connected SSE file descriptors. When an event is emitted, iterate and write to all. Handle disconnects gracefully (remove dead FDs on EPIPE).

### 4. Migration Path

The current webchat UI uses:
- `POST /api/chat/send` → migrate to `POST /api/sessions/{id}/message`
- Inline SSE in the send response → migrate to `GET /api/sessions/{id}/events` as a separate connection
- `GET /api/chat/session` → migrate to `GET /api/sessions/{id}`
- `POST /api/chat/clear` → migrate to `DELETE /api/sessions/{id}` + `POST /api/sessions`

Old endpoints can be kept as thin wrappers during transition.

### Changes

| File | Change |
|------|--------|
| `src/webchat.c` | Add session CRUD endpoints, SSE broadcast, multi-client support |
| `src/dashboard.c` | Add session list/detail widgets that subscribe to session events |

## Acceptance Criteria

- [ ] `POST /api/sessions` creates a session and returns `{session_id: "..."}` 
- [ ] `GET /api/sessions` returns a list with id, created_at, message_count per session
- [ ] `GET /api/sessions/{id}/events` streams SSE with keepalive
- [ ] Two browser tabs subscribed to the same session both receive `text_delta` events
- [ ] Dashboard can show real-time delegate progress by subscribing to session events
- [ ] `DELETE /api/sessions/{id}` cleans up session state
- [ ] Old `/api/chat/*` endpoints continue working (backwards compatible)

## Owner and Effort

- **Owner:** aimee maintainer
- **Effort:** M (SSE infrastructure exists; main work is session CRUD and multi-client broadcast)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** New endpoints available immediately. Webchat UI migrated in a follow-up PR. Old endpoints kept as aliases.
- **Rollback:** Remove new endpoints; webchat continues using old `/api/chat/*` routes.
- **Blast radius:** Webchat server only. CLI, delegates, MCP unaffected.

## Test Plan

- [ ] Unit tests: session create/list/get/delete lifecycle
- [ ] Integration tests: SSE subscription receives events during a chat turn
- [ ] Integration tests: two concurrent SSE subscribers both receive events
- [ ] Failure injection: client disconnect mid-stream doesn't crash server
- [ ] Manual verification: open two browser tabs, observe shared session streaming

## Operational Impact

- **Metrics:** `webchat.sessions.active`, `webchat.sse.connections` gauges
- **Logging:** INFO on session create/close, DEBUG on SSE connect/disconnect
- **Alerts:** None
- **Disk/CPU/Memory:** One FD per SSE subscriber per session. Memory: session history (~10-100KB per session)

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Session API | P2 | M | High — enables dashboard integration, multi-client observation, proper session lifecycle |

## Trade-offs

- **WebSocket** vs **SSE**: WebSocket enables bidirectional communication but adds complexity (upgrade handshake, frame parsing, ping/pong). SSE is sufficient since messages are sent via POST and events flow server-to-client. WebSocket can be added later if bidirectional streaming becomes needed.
- **In-memory sessions** vs **persistent sessions**: In-memory is simpler and matches claw-code's approach. Persistent sessions (tied to session-resume proposal) can be layered on later.
- **Per-session thread** vs **event loop**: The current webchat uses per-connection threads. Multi-client SSE is easier with an event loop (epoll/poll), but refactoring the threading model is out of scope. Use a mutex-protected FD list within the existing thread model.
