# Proposal: Request Size Ceilings and JSON Framing Guardrails

## Problem

The server accepts messages up to 16MB (`SERVER_MAX_MSG_SIZE = 16 * 1024 * 1024`,
`server.h:14`), the CLI client buffers up to 65KB (`CLIENT_READ_BUF_SIZE`,
`cli_client.h:8`), and the MCP server reads 65KB lines (`MCP_LINE_MAX`,
`mcp_server.c:14`). However:

1. There are no per-method size limits — a `memory.store` call gets the same
   16MB ceiling as a `tool.execute` response.
2. No JSON depth or field-count limits — deeply nested or wide JSON payloads can
   cause stack exhaustion or excessive memory allocation in cJSON parsing
   (`server_dispatch()` at `server.c:206`).
3. No explicit rejection codes — oversized messages are silently truncated or
   cause parse failures with generic error messages.

## Goals

- Hard caps for message size, JSON nesting depth, and field counts per method class.
- Early rejection with explicit, machine-readable error codes.
- Protection against memory pressure from malformed or adversarial input.

## Approach

### 1. Per-method size limits

Define a size table mapping method prefixes to maximum payload sizes:

```c
static const struct { const char *prefix; size_t max; } method_limits[] = {
    {"memory.",   256 * 1024},   /* 256KB for memory operations */
    {"tool.",     4 * 1024 * 1024}, /* 4MB for tool I/O */
    {"delegate",  1 * 1024 * 1024}, /* 1MB for delegation */
    {"chat.",     512 * 1024},   /* 512KB for chat messages */
    {NULL,        256 * 1024},   /* default */
};
```

Check message length against the method-specific limit *before* JSON parsing.
Extract the method name from the raw line using a lightweight prefix scan (the
method field is always near the start of the JSON object).

### 2. JSON depth and width limits

Add pre-parse or parse-time limits:

- **Max nesting depth:** 32 levels (prevents stack exhaustion in recursive cJSON walk).
- **Max object field count:** 256 per object (prevents hash collision abuse in cJSON's linked-list model).

Implement as a cJSON parse wrapper that tracks depth during parsing and rejects
early.

### 3. Structured rejection

Return a specific error code for size/framing violations:

```json
{"status": "error", "code": "PAYLOAD_TOO_LARGE", "message": "...", "limit": 262144}
{"status": "error", "code": "PAYLOAD_MALFORMED", "message": "JSON depth exceeds 32"}
```

### Changes

| File | Change |
|------|--------|
| `src/server.c` | Add `check_message_limits()` before `cJSON_Parse()` in `server_dispatch()` |
| `src/server.c` | Add `cjson_parse_bounded()` wrapper with depth/width tracking |
| `src/server.h` | Define per-method limit table and `PAYLOAD_*` error codes |
| `src/mcp_server.c` | Apply same depth/width limits to MCP JSON parsing |

## Acceptance Criteria

- [ ] Messages exceeding per-method size limit are rejected before JSON parsing
- [ ] JSON documents deeper than 32 levels are rejected with `PAYLOAD_MALFORMED`
- [ ] JSON objects wider than 256 fields are rejected
- [ ] Rejection responses include machine-readable error code and limit value
- [ ] Normal operations (memory store, tool execute, delegate) work within limits

## Owner and Effort

- **Owner:** TBD
- **Effort:** S-M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Ships with next binary. Limits are compile-time constants; can be made configurable later.
- **Rollback:** Revert commit. Restores 16MB uniform limit and unbounded parsing.
- **Blast radius:** Requests that legitimately exceed new limits will be rejected. Tool output is the most likely to hit the 4MB cap.

## Test Plan

- [ ] Unit test: message at exactly the limit passes; message 1 byte over is rejected
- [ ] Unit test: JSON with depth 32 parses; depth 33 is rejected
- [ ] Unit test: JSON object with 256 fields parses; 257 is rejected
- [ ] Integration test: normal `memory.store`, `tool.execute`, `delegate` operations succeed
- [ ] Fuzz test: random-length and random-depth JSON payloads produce correct accept/reject

## Operational Impact

- **Metrics:** Counter for rejected-by-size and rejected-by-depth events (future).
- **Logging:** Log rejected payloads at WARN level with method, size, and limit.
- **Alerts:** None.
- **Disk/CPU/Memory:** Reduces peak memory — 16MB allocations become impossible for non-tool methods.

## Priority

P0 — prevents memory exhaustion and protocol abuse.

## Trade-offs

**Why not streaming JSON parsing?** cJSON is an in-memory DOM parser. Switching to
a streaming parser (e.g., yajl) is a larger change. The bounded-parse wrapper
achieves the safety goal with minimal disruption.

**Why compile-time limits?** Operator-configurable limits add complexity for
marginal gain. The chosen values are generous for all known use cases. Can be
made configurable if real-world usage demonstrates a need.
