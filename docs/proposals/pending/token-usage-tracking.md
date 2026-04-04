# Proposal: Token Usage Tracking and Budget Display

## Problem

Aimee's webchat and CLI chat modes have no visibility into token consumption. When an agent makes tool calls, engages in long conversations, or delegates tasks, the user has no way to see:
- How many tokens were used per request/response
- Cumulative token usage for the session
- How close the conversation is to context limits
- Cost estimates for API-based providers

This makes it impossible to:
- Budget API costs for cloud-hosted conversations
- Detect when context is getting dangerously full (before the LLM starts losing context)
- Compare token efficiency between different approaches
- Identify wasteful tool calls that consume disproportionate tokens

ayder-cli implements comprehensive token tracking:
- **Per-request tracking**: Input tokens, output tokens, total tokens reported after each LLM call
- **Cumulative session tracking**: Running total displayed in the TUI status bar
- **Token estimation**: For providers that don't report usage, estimates via tiktoken or character-ratio heuristics
- **Context utilization**: Shows what percentage of the context window is used
- **Verbose mode**: Logs token counts to stderr for CLI mode

Evidence:
- `webchat.c` makes API calls but discards usage data from responses
- `cmd_chat.c` processes SSE responses but doesn't extract token counts
- No token tracking exists anywhere in the codebase
- The dashboard (`dashboard.c`) shows delegation metrics but not token usage
- Delegates report `tool_calls` count but not token consumption

## Goals

- Per-request token usage (input/output/total) is tracked for all LLM calls in webchat and CLI chat.
- Cumulative session token usage is visible in the webchat UI and CLI output.
- Context utilization percentage is displayed, showing how full the context window is.
- Token usage is included in delegation stats and the dashboard.
- Token estimation works for providers that don't report usage natively.
- Cost estimates are shown for providers with known pricing.

## Approach

### 1. Token extraction from API responses

Both OpenAI and Anthropic responses include usage data:

**OpenAI format** (used by webchat/CLI chat):
```json
{
  "usage": {
    "prompt_tokens": 1234,
    "completion_tokens": 567,
    "total_tokens": 1801
  }
}
```

**Anthropic format**:
```json
{
  "usage": {
    "input_tokens": 1234,
    "output_tokens": 567
  }
}
```

Extract these from the final SSE chunk or response body.

### 2. Session token tracker

```c
typedef struct {
    int64_t total_input_tokens;
    int64_t total_output_tokens;
    int64_t total_tokens;
    int request_count;
    int context_window_size;      // from model config
    int estimated_context_used;   // running estimate
} token_tracker_t;

void token_tracker_record(token_tracker_t *t, int input_tokens, int output_tokens);
float token_tracker_utilization(const token_tracker_t *t);
```

### 3. Token estimation fallback

For providers or responses that don't include usage data, estimate:

```c
// Estimate tokens from text using character ratio
// English text: ~4 chars/token, code: ~3.5 chars/token
int estimate_tokens(const char *text, int is_code);
```

### 4. Webchat integration

Add token usage to the webchat UI in two places:

**Status bar** (always visible):
```
Tokens: 3,456 | Context: 42% | Cost: ~$0.03
```

**Per-message** (in SSE events):
```json
{
  "event": "usage",
  "data": {"input": 1234, "output": 567, "total": 1801, "session_total": 3456, "utilization": 0.42}
}
```

### 5. CLI chat integration

In CLI chat, show token usage after each response (always, not just verbose):
```
[1,801 tokens | session: 3,456 | context: 42%]
```

### 6. Dashboard integration

Add a "Token Usage" card to the dashboard showing:
- Session token total
- Tokens by delegate
- Average tokens per delegation

### 7. Delegation stats

Include token counts in delegation attempt records:

```c
// In delegation attempt logging
record_attempt(..., int input_tokens, int output_tokens);
```

### Changes

| File | Change |
|------|--------|
| `src/token_tracker.c` (new) | Token tracking, estimation, utilization calculation |
| `src/headers/token_tracker.h` (new) | Public token tracker API |
| `src/webchat.c` | Extract usage from API responses, send SSE usage events, show in status |
| `src/webchat_assets.c` | Add token display to webchat UI status bar and per-message |
| `src/cmd_chat.c` | Extract usage from API responses, display after each response |
| `src/dashboard.c` | Add token usage card to dashboard, include in delegation metrics |
| `src/agent_coord.c` | Pass token tracker to delegates, aggregate in delegation stats |
| `src/config.c` | Parse `context_window_size` per model for utilization calculation |

## Acceptance Criteria

- [ ] Per-request token usage is extracted from OpenAI and Anthropic API responses
- [ ] Cumulative session token total is tracked and displayed in webchat and CLI chat
- [ ] Context utilization percentage is calculated and displayed
- [ ] Token estimation works when provider doesn't report usage
- [ ] Dashboard shows token usage per session and per delegation
- [ ] Delegation stats include token counts
- [ ] Cost estimates are shown for providers with known pricing (optional, P3)

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** M (2-3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Token tracking is always active. Display is always visible in webchat/CLI (no opt-out needed since it's informational only).
- **Rollback:** Remove token extraction and display. No behavioral change.
- **Blast radius:** Zero — purely observational. Doesn't affect tool execution or agent behavior.

## Test Plan

- [ ] Unit tests: token extraction from OpenAI/Anthropic response formats, token estimation, utilization calculation
- [ ] Integration tests: webchat session with token tracking, verify SSE usage events
- [ ] Integration tests: CLI chat with token display after each response
- [ ] Failure injection: response with no usage data (fallback to estimation), malformed usage field
- [ ] Manual verification: run a webchat conversation, verify token counts in status bar match API billing

## Operational Impact

- **Metrics:** `session_tokens_total`, `delegation_tokens_total{role="code"}`, `token_estimation_fallback_total`
- **Logging:** Token counts at DEBUG per request
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — a few integers per request

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Token extraction from responses | P2 | S | High — enables everything |
| Session tracking + display | P2 | S | High — immediate cost visibility |
| Context utilization | P2 | S | High — prevents context overflow |
| Dashboard integration | P3 | S | Medium — delegation visibility |
| Token estimation fallback | P3 | S | Low — only needed for non-reporting providers |
| Cost estimates | P3 | S | Low — nice-to-have |

## Trade-offs

- **Why not just check the API billing dashboard?** API dashboards show total usage, not per-session or per-request breakdowns. In-session tracking shows where tokens are spent, enabling real-time optimization (e.g., "that tool call used 5K tokens, I should use a more targeted query").
- **Why estimate when providers don't report?** Claude CLI forwarding and some Ollama models don't include usage data. Estimation gives approximate visibility rather than no visibility.
- **Why always display instead of opt-in?** Token awareness should be the default. Hiding costs discourages cost-conscious usage. The display is small and unobtrusive.

## Source Reference

Implementation reference: ayder-cli — token tracking in `DefaultContextManager.update_from_response()`, `ContextStats.total_tokens`, `NormalizedStreamChunk.usage` field, `StatusBar.update_token_usage()` widget, and `TokenCounter` class with tiktoken/character-ratio estimation.
