# Proposal: Streaming REPL with Real-Time Tool Feedback

## Problem

Aimee's built-in chat (`cmd_chat.c`, `cli_client.c`) collects the full LLM response before displaying it. There is no token-by-token streaming, no real-time tool invocation feedback, and no spinner/progress indication during long operations. Users see nothing for 10-30 seconds during complex turns, then get a wall of text.

The claw-code project (github.com/ultraworkers/claw-code) implements a streaming REPL in `claw-cli/src/app.rs` with:
- **TextDelta** events rendered token-by-token as they arrive
- **ToolCallStart** events showing tool name and input immediately
- **ToolCallResult** events rendering tool output as markdown
- **Usage** events tracking token consumption per turn
- Spinners that transition between "thinking" and "streaming" states
- Output modes: text, JSON, NDJSON for piping

Aimee already has SSE streaming infrastructure in `server_session.c` and provider-level streaming in `cli_client.c`, but the REPL display layer doesn't use it — responses are buffered and printed whole.

## Goals

- Users see assistant text appear token-by-token as it streams.
- Tool invocations are announced in real-time (name, abbreviated input) before execution.
- Tool results are displayed immediately after completion, before the assistant continues.
- A spinner or progress indicator is visible during model thinking and tool execution.
- Token usage (input/output/cache) is displayed per-turn when `--verbose` is set.

## Approach

### 1. Stream Event Types

Define a callback-based event system for the REPL:

```c
enum repl_event_type {
    REPL_EVT_TEXT_DELTA,    /* partial text token */
    REPL_EVT_TOOL_START,    /* tool name + input */
    REPL_EVT_TOOL_RESULT,   /* tool output + error flag */
    REPL_EVT_USAGE,         /* token counts */
    REPL_EVT_THINKING,      /* model is processing */
};
```

### 2. Display Rendering

- **Text deltas**: Write directly to stdout with `fwrite()` + `fflush()`, no buffering.
- **Tool start**: Print `[tool: bash] rm -rf ... ` (truncated input) with a spinner.
- **Tool result**: Render output, capped at terminal height. Errors in red.
- **Spinner**: Simple `|/-\` rotation on a 100ms timer using `SIGALRM` or a background thread.

### 3. Provider Integration

The streaming SSE parser in `cli_client.c` already handles `data:` events. Wire each SSE chunk to the REPL event callback instead of accumulating into a buffer.

### 4. Output Modes

Support `--output=text|json|ndjson`:
- **text** (default): Human-readable streaming with colors and spinners.
- **json**: Buffer and emit a single JSON object per turn (current behavior).
- **ndjson**: Emit one JSON line per event, suitable for piping to other tools.

### Changes

| File | Change |
|------|--------|
| `src/cli_client.c` | Add `repl_event_callback_t` parameter to chat functions; emit events instead of buffering |
| `src/cmd_chat.c` | Implement REPL event handler with streaming display |
| `src/render.c` | Add `render_tool_start()`, `render_tool_result()`, `render_spinner()` |
| `src/headers/cli_client.h` | Define `repl_event_type` enum and callback typedef |

## Acceptance Criteria

- [ ] `aimee chat "explain this file"` streams text token-by-token visibly
- [ ] Tool invocations show `[tool: name]` before execution and result after
- [ ] A spinner is visible during model thinking (before first token)
- [ ] `--output=ndjson` emits one JSON line per stream event
- [ ] Ctrl-C during streaming cancels the current turn cleanly
- [ ] Non-TTY output (piped) disables spinners and colors automatically

## Owner and Effort

- **Owner:** aimee maintainer
- **Effort:** M (plumbing exists in SSE layer; main work is display rendering and callback wiring)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Streaming display is the new default for `aimee chat`. Old buffered behavior available via `--output=json`.
- **Rollback:** Revert the `cmd_chat.c` event handler to buffer-and-print.
- **Blast radius:** Only affects `aimee chat` interactive mode. Server, delegates, MCP unaffected.

## Test Plan

- [ ] Unit tests: event callback receives correct event types in correct order
- [ ] Integration tests: `aimee chat --output=ndjson` piped to `jq` validates event schema
- [ ] Failure injection: network drop mid-stream produces clean error, not partial output
- [ ] Manual verification: visually confirm streaming + spinner in terminal

## Operational Impact

- **Metrics:** None (client-side only)
- **Logging:** DEBUG for each stream event
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — replacing buffer accumulation with direct writes

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Streaming REPL | P2 | M | High — transforms UX of built-in chat from "wait and dump" to interactive |

## Trade-offs

- **ncurses/TUI** was considered but adds a heavy dependency and complexity for marginal gain. Raw ANSI escape codes are sufficient for spinners and colors.
- **Async I/O (epoll/kqueue)** for spinner timing was considered but `SIGALRM` or a simple thread is simpler for a single spinner.
- The NDJSON output mode adds code but is essential for tooling integration (e.g., piping to `jq` or a monitoring dashboard).
