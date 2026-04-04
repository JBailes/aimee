# Proposal: Live Session Stats Display

## Problem

During a chat or webchat session, users have no visibility into resource consumption: how many turns have elapsed, how many tokens consumed, how many tool calls succeeded vs. failed, or how much context window remains. This makes it hard to gauge whether a session is running efficiently or spiraling.

Mistral-vibe implements a `/status` slash command that displays live agent stats: turns, tokens (prompt/completion), pricing, tool call counts (succeeded/failed/rejected/agreed), and context window usage percentage.

## Goals

- Users can view live session stats at any time during a chat session.
- Stats include: turn count, token usage (prompt + completion), tool calls (success/fail/skip), context window usage percentage.
- Available in both CLI (`/status` or `aimee chat --stats`) and webchat (stats panel or API endpoint).
- Stats update in real-time in webchat.

## Approach

### Data Collection

Aimee already tracks `prompt_tokens`, `completion_tokens`, `total_prompt_tokens`, `total_completion_tokens` in `agent_result_t` and `agent_stats_t`. Extend this with:
- Tool call counters: `tools_succeeded`, `tools_failed`, `tools_skipped`
- Turn counter: increment per agent loop iteration
- Context usage: compute from total tokens vs. model's context window size

### CLI Display

Add a `/status` handler in the chat loop that prints a compact stats block:

```
Session Stats
  Turns:    12
  Tokens:   prompt 8,432 / completion 2,104
  Tools:    18 succeeded, 2 failed, 1 skipped
  Context:  42% used (10,536 / 25,000)
```

### Webchat Display

Add a collapsible stats panel in the webchat sidebar. Expose via SSE event (`stats_update`) sent after each turn. Also available via `GET /api/session/<id>/stats`.

### Changes

| File | Change |
|------|--------|
| `src/headers/agent_types.h` | Extend `agent_stats_t` with tool call counters, context percentage |
| `src/agent.c` | Increment stats during agent loop |
| `src/agent_tools.c` | Increment tool success/fail/skip counters |
| `src/cmd_chat.c` | Add `/status` slash command handler |
| `src/webchat.c` | Add stats SSE event and API endpoint |
| `src/webchat_assets.c` | Add stats panel to webchat sidebar |

## Acceptance Criteria

- [ ] `/status` in CLI chat shows turn count, token usage, tool call counts, context percentage
- [ ] Webchat stats panel updates after each turn via SSE
- [ ] `GET /api/session/<id>/stats` returns JSON stats
- [ ] Context usage percentage is calculated from model's known context window
- [ ] Tool call counters distinguish success, failure, and skip
- [ ] Stats reset on new session, persist through session resume

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Additive. New slash command and webchat panel.
- **Rollback:** Remove handler. No persistent state changes.
- **Blast radius:** Display only. No functional impact.

## Test Plan

- [ ] Unit tests: stats accumulation across turns
- [ ] Integration tests: multi-turn session, verify stats accuracy
- [ ] Manual verification: `/status` output and webchat panel

## Operational Impact

- **Metrics:** None (stats are session-scoped, not exported)
- **Logging:** None
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — a few integers in session state

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Live Session Stats | P3 | S | Medium — visibility into session health and cost |

## Trade-offs

**Alternative: Integrate with the OTEL proposal and show stats in Grafana.** Complementary, not competing. `/status` gives in-session visibility; OTEL gives cross-session analytics.

**Known limitation:** Context window size must be known per model. Maintain a lookup table in config; fall back to a conservative default if unknown.
