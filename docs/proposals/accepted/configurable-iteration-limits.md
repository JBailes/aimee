# Proposal: Configurable Iteration Limits for Agent Loops

## Problem

When aimee delegates tasks to agents (codex, ollama, or the webchat chat loop), there is no configurable cap on how many tool-call iterations the agent can perform per message. An agent stuck in a retry loop, hallucinating tool calls, or exploring an unproductive path can burn tokens and time indefinitely until the session timeout kills it.

ayder-cli implements a `max_iterations` setting (default 10, range 1-100) that caps the number of LLM→tool→LLM cycles per user message. When the limit is reached, the loop returns the last assistant message and stops. This is separate from session timeouts — it's a per-turn budget that prevents runaway agentic loops without killing the session.

Evidence:
- `agent_coord.c` delegates with a timeout (`agent_timeout`) but no iteration cap
- The webchat chat loop (`webchat.c`) and CLI chat loop (`cmd_chat.c`) loop until `finish_reason` is `stop` with no iteration guard
- Delegates that enter retry loops (e.g., failing verification command → retrying → failing again) burn tokens until timeout
- The delegation attempt log shows cases where delegates used 20+ tool calls on tasks that should have taken 3-5

## Goals

- A configurable `max_iterations` setting caps the number of tool-call rounds per user message in all agent loops (webchat, CLI chat, delegates).
- The limit is separate from session timeout — it's a per-turn budget.
- When the limit is reached, the agent returns its last response rather than being killed.
- The limit is configurable globally and per-delegate profile.
- Iteration count is visible in the webchat UI status bar and delegate stats.

## Approach

### 1. Global config

Add to aimee config:

```
[agent]
max_iterations = 15          # default per-turn iteration cap
max_iterations_delegate = 25 # cap for delegate sessions (higher for autonomous work)
```

### 2. Chat loop guard

In both `cmd_chat.c` and `webchat.c`, add an iteration counter inside the tool-call loop:

```c
int iterations = 0;
while (has_tool_calls && iterations < max_iterations) {
    execute_tool_calls(...);
    send_to_llm(...);
    iterations++;
}
if (iterations >= max_iterations) {
    // Return last content as final response
    log_warn("iteration limit reached (%d) for message", max_iterations);
}
```

### 3. Delegate integration

In `agent_coord.c`, pass the iteration limit to delegate sessions. Delegates that hit the limit return their partial result rather than timing out.

### 4. Webchat status

In the webchat UI, show iteration count in the status area:

```
Turn 3/15 | 2 tool calls
```

### 5. CLI output

In CLI chat, show a notice when the limit is approaching:

```
⚠ Iteration 14/15 — approaching limit
```

### Changes

| File | Change |
|------|--------|
| `src/config.c` | Parse `max_iterations` and `max_iterations_delegate` settings |
| `src/cmd_chat.c` | Add iteration counter to chat loop, enforce limit |
| `src/webchat.c` | Add iteration counter to chat loop, enforce limit, send count via SSE |
| `src/webchat_assets.c` | Display iteration count in webchat status bar |
| `src/agent_coord.c` | Pass iteration limit to delegate sessions |
| `src/agent_config.c` | Support per-agent iteration limit override |

## Acceptance Criteria

- [ ] `max_iterations` config setting caps tool-call rounds in CLI chat, webchat, and delegate sessions
- [ ] Hitting the limit returns the last assistant response (not an error)
- [ ] Webchat UI shows current iteration count (e.g., "Turn 3/15")
- [ ] Per-delegate iteration limits override the global default
- [ ] Iteration count is included in delegate stats and attempt logs

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S (1-2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Default limit of 15 iterations for interactive, 25 for delegates. Existing sessions are unaffected until restart.
- **Rollback:** Set `max_iterations = 0` to disable (or a very high number). Remove config parsing and loop guard.
- **Blast radius:** An overly low limit could truncate legitimate long-running tool sequences. Mitigation: default is generous (15/25), and the agent returns partial results rather than erroring.

## Test Plan

- [ ] Unit tests: iteration counter increments, limit enforcement, partial result return
- [ ] Integration tests: delegate hits iteration limit, returns summary; webchat shows count
- [ ] Failure injection: max_iterations=1 (immediate stop after first tool call), max_iterations=0 (disabled/unlimited)
- [ ] Manual verification: set max_iterations=5, ask agent to do a 10-step task, verify it stops at 5 and returns partial result

## Operational Impact

- **Metrics:** `iterations_per_turn_histogram`, `iteration_limit_hit_total`
- **Logging:** Limit hit at WARN, iteration count at DEBUG per turn
- **Alerts:** High `iteration_limit_hit_total` rate suggests the default is too low
- **Disk/CPU/Memory:** Zero — this is a counter check in an existing loop

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Loop guard in chat loops | P1 | S | High — prevents token waste |
| Config setting | P2 | S | Medium — tunability |
| Webchat iteration display | P3 | S | Low — observability |
| Per-delegate overrides | P3 | S | Low — fine-tuning |

## Trade-offs

- **Why not just use timeouts?** Timeouts are wall-clock based and don't account for fast vs. slow iterations. An agent can burn 50 fast tool calls in 30 seconds. Iteration limits are a direct budget on LLM calls, which is the actual cost driver.
- **Why separate limits for interactive vs. delegate?** Delegates often need more iterations for autonomous multi-step tasks. Interactive users are watching and can re-prompt, so a lower limit with partial results is preferable.
- **Why return partial results instead of erroring?** The agent may have accomplished useful work in the first N iterations. Returning that work is better than discarding it.

## Source Reference

Implementation reference: ayder-cli `loops/chat_loop.py` — iteration counter in `run()` method, `max_iterations` in `ChatLoopConfig`, `app.max_iterations` config field (range 1-100, default 10).
