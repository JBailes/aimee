# Proposal: Turn Summary Narration

## Problem

During multi-turn agent sessions (chat and webchat), the user sees raw tool output scrolling past — file diffs, command output, API responses. After a complex turn with 5+ tool calls, it's hard to understand what the agent actually accomplished. The user must mentally reconstruct the narrative from tool results.

Mistral-vibe implements a `TurnSummaryTracker` that, after each agent turn, generates a concise natural-language summary of what happened (e.g., "Modified `src/auth.c` to add rate limiting, ran tests — 3 passed, 1 failed on timeout"). This summary is shown to the user and optionally stored for session history.

## Goals

- After each agent turn, display a concise 1-2 sentence summary of what happened.
- Summary generation is asynchronous — it doesn't block the next user input.
- Works in both CLI (printed below tool output) and webchat (displayed as a collapsible summary card).
- Summaries are stored in the session for history/resume context.

## Approach

After each agent turn completes, collect the turn's content: user message, tool calls (names + abbreviated args), tool results (abbreviated), and assistant response. Send this to a fast/cheap model (the delegate's draft tier) with a summarization prompt. Display the result.

### Summary Generation

```
Input to summarizer:
  - User request: "Add rate limiting to the auth endpoint"
  - Tools called: write_file(src/auth.c), bash(make test)
  - Tool results: [file written, 3/4 tests passed]
  - Assistant said: "I've added rate limiting..."

Output: "Modified src/auth.c with token-bucket rate limiter. Tests: 3/4 passing (timeout failure in test_concurrent_auth)."
```

### Display

**CLI:** Printed as a dimmed/italic line after the assistant response:
```
  ℹ Modified src/auth.c with rate limiter. 3/4 tests passing.
```

**Webchat:** Collapsible summary card between the tool output and the next input area.

### Integration Points

- **Agent loop (`agent.c` / `cmd_chat.c`)**: After each turn, fire async summary generation.
- **Webchat (`webchat.c`)**: Send summary as an SSE event after tool results.
- **Session storage**: Store summaries in session messages for resume context.

### Changes

| File | Change |
|------|--------|
| `src/headers/agent.h` | Add turn summary types and async generation API |
| `src/agent.c` | Collect turn content after tool execution, dispatch summary |
| `src/cmd_chat.c` | Display turn summary in CLI after each turn |
| `src/webchat.c` | Send turn summary SSE event; display as collapsible card |
| `src/webchat_assets.c` | Add summary card UI component |
| `src/agent_context.c` | Summary generation using draft-tier delegate |

## Acceptance Criteria

- [ ] After each turn with tool calls, a 1-2 sentence summary is displayed
- [ ] Summary generation doesn't block user input (async in CLI, SSE in webchat)
- [ ] Summary includes: files modified, commands run, test results, errors
- [ ] Summaries stored in session for resume context
- [ ] CLI summary renders as dimmed text below assistant response
- [ ] Webchat summary renders as collapsible card
- [ ] Summary generation uses cheapest available model tier
- [ ] Can be disabled via config (`turn_summary.enabled = false`)

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (1-2 days)
- **Dependencies:** None (uses existing delegate infrastructure for LLM call)

## Rollout and Rollback

- **Rollout:** Enabled by default. Disable via config.
- **Rollback:** Toggle config flag. No persistent state impact.
- **Blast radius:** Cosmetic — only affects display. Summary failures are silently ignored.

## Test Plan

- [ ] Unit tests: turn content collection, summary prompt formatting
- [ ] Integration tests: multi-tool turn produces accurate summary
- [ ] Failure injection: summary model unavailable — verify no impact on chat flow
- [ ] Manual verification: CLI and webchat both display summaries

## Operational Impact

- **Metrics:** `turn_summary_generated`, `turn_summary_latency_ms`
- **Logging:** DEBUG for summary generation, WARN on failures
- **Alerts:** None
- **Disk/CPU/Memory:** One cheap LLM call per turn (~100 tokens). Negligible.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Turn Summary Narration | P3 | S | Medium — UX polish that helps users track agent progress |

## Trade-offs

**Alternative: Rule-based summaries without LLM.** Template like "Modified N files, ran M commands." Cheaper but less informative — misses semantic content like "added rate limiting."

**Alternative: Only show summaries for turns with 3+ tool calls.** Good optimization for reducing noise. Could be a config threshold.

**Known limitation:** Summary quality depends on the draft-tier model. Very cheap models may produce generic summaries. Acceptable tradeoff given the cost sensitivity.
