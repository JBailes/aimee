# Proposal: Incremental SSE Parser with Split-Frame Handling

## Problem

SSE (Server-Sent Events) parsing looks trivial but handling chunked TCP delivery — where an SSE frame is split across two or more `read()` calls — is a common source of bugs. Aimee's streaming response handling likely has ad-hoc SSE parsing that may break when frames are split at arbitrary byte boundaries.

This affects all streaming paths: CLI delegates, webchat real-time updates, and MCP tool calls — all consume SSE from LLM providers.

The `soongenwong/claudecode` repo implements two battle-tested incremental SSE parsers at `rust/crates/api/src/sse.rs` and `rust/crates/runtime/src/sse.rs`.

## Goals

- SSE frames split across TCP read boundaries are correctly reassembled.
- Both CRLF and LF line endings are handled.
- Multi-line `data:` fields are concatenated correctly.
- Comment lines (starting with `:`) are filtered.
- `[DONE]` sentinel and keepalive pings are detected.
- Parser is reusable across all streaming consumers (API client, MCP client, webchat SSE) in both CLI and webchat.

## Approach

### Parser State Machine

```c
typedef struct {
    char *buffer;
    size_t buffer_len;
    size_t buffer_cap;
} sse_parser_t;

/* Feed raw bytes from read(). Returns completed events. */
int sse_parser_feed(sse_parser_t *p, const char *chunk, size_t len,
                    sse_event_t *events_out, int max_events);
```

Key behaviors:
- Buffers incomplete lines across `feed()` calls
- Splits on `\n\n` or `\r\n\r\n` (event boundary)
- Strips `data: ` prefix, concatenates multi-line data with `\n`
- Detects `event:` field for event type
- Filters `:` comment lines and empty keepalives

### Changes

| File | Change |
|------|--------|
| `src/sse_parser.c` (new) | Incremental SSE parser: buffering, frame detection, field parsing |
| `src/headers/sse_parser.h` (new) | Public API |
| `src/agent_http.c` | Replace ad-hoc SSE parsing with `sse_parser_feed()` |
| `src/server_forward.c` | Use parser for forwarded SSE streams |
| `src/webchat.c` | Use parser for any SSE consumption in webchat backend |

## Acceptance Criteria

- [ ] Frame split at arbitrary byte boundary is correctly reassembled
- [ ] Both `\n\n` and `\r\n\r\n` event boundaries work
- [ ] Multi-line `data:` fields concatenated with newlines
- [ ] Comment lines and keepalive pings filtered
- [ ] `[DONE]` sentinel detected
- [ ] Works for all streaming consumers: CLI, webchat, MCP

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S (1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Drop-in replacement for existing SSE parsing.
- **Rollback:** Revert to previous parsing code.
- **Blast radius:** Incorrect parsing breaks streaming entirely. Thorough tests mitigate.

## Test Plan

- [ ] Unit tests: split frame at every byte offset, multi-line data, CRLF/LF, comment filtering
- [ ] Integration tests: stream from mock server with artificial chunking
- [ ] Manual verification: long streaming response over high-latency connection

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Incremental SSE parser | P2 | S | High — correctness for all streaming |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/api/src/sse.rs` and `rust/crates/runtime/src/sse.rs`.
