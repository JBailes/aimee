# Proposal: Token Usage and Cost Tracking

## Problem

Aimee's built-in chat and delegation system have no visibility into token consumption or cost. Users cannot see:
- How many tokens a chat turn or delegation consumed
- Cumulative token usage across a session
- Estimated cost in USD for a session or delegation
- Cache hit rates (important for prompt caching with Anthropic)
- Whether a delegate is approaching context limits

The claw-code project implements comprehensive tracking in `runtime/usage.rs`:
- **Per-turn and cumulative tracking**: input, output, cache_creation, cache_read tokens
- **Model-specific pricing**: Different rates for Haiku/Sonnet/Opus with separate input/output/cache pricing
- **Cost estimation**: `estimate_cost_usd_with_pricing()` calculates USD from token counts
- **Session reconstruction**: Rebuilds usage from conversation history
- **Summary display**: Formatted output with `summary_lines_for_model()`

Aimee tracks delegation metrics (turns, tool calls, latency) in `agent_coord.c` but has no token-level granularity and no cost estimation.

## Goals

- Token usage (input, output, cache write, cache read) is tracked per-turn and cumulatively.
- Estimated cost in USD is calculated using model-specific pricing.
- `/cost` or `/status` in chat displays current session token usage and cost.
- Delegation summaries include token counts and estimated cost.
- Dashboard shows token usage and cost per delegation and aggregate.

## Approach

### 1. Token Extraction from Provider Responses

Both OpenAI and Anthropic return usage in their SSE streams:
- **OpenAI**: `usage` field in the final chunk: `{prompt_tokens, completion_tokens, total_tokens}`
- **Anthropic**: `message_delta` event with `usage: {input_tokens, output_tokens}` and `message_start` with cache tokens

Extract these from the SSE parsers in `cmd_chat.c` and `webchat.c`.

### 2. Usage Accumulator

```c
typedef struct {
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t cache_write_tokens;
    int64_t cache_read_tokens;
} token_usage_t;

typedef struct {
    token_usage_t last_turn;
    token_usage_t cumulative;
    int turn_count;
} usage_tracker_t;
```

### 3. Cost Estimation

Pricing table per model family:

| Model | Input $/M | Output $/M | Cache Write $/M | Cache Read $/M |
|-------|-----------|------------|-----------------|----------------|
| claude-haiku | 0.80 | 4.00 | 1.00 | 0.08 |
| claude-sonnet | 3.00 | 15.00 | 3.75 | 0.30 |
| claude-opus | 15.00 | 75.00 | 18.75 | 1.50 |
| gpt-4o | 2.50 | 10.00 | — | — |
| gpt-4o-mini | 0.15 | 0.60 | — | — |
| gemini-2.5-pro | 1.25 | 10.00 | — | 0.315 |

```c
double estimate_cost_usd(const token_usage_t *usage, const char *model);
```

### 4. Display Integration

- **CLI chat**: `/cost` or `/status` shows usage summary
- **Webchat**: Include usage in SSE events; display in chat footer
- **Delegations**: Add `input_tokens`, `output_tokens`, `estimated_cost` to delegation results in `agent_coord.c`
- **Dashboard**: Add cost column to delegations table and cost aggregate to metrics

### Changes

| File | Change |
|------|--------|
| `src/cmd_chat.c` | Extract usage from SSE responses; accumulate in `usage_tracker_t` |
| `src/webchat.c` | Same extraction; include in SSE events to browser |
| `src/agent_coord.c` | Record token usage in delegation results |
| `src/dashboard.c` | Add cost column to delegations, add aggregate cost card |
| `src/render.c` | Add `render_usage_summary()` for formatted display |
| `src/headers/aimee.h` | Define `token_usage_t`, `usage_tracker_t`, pricing API |

## Acceptance Criteria

- [ ] After a chat turn, `/cost` shows input/output tokens and estimated USD
- [ ] Cumulative usage across multiple turns is accurate
- [ ] Cache tokens (Anthropic) are tracked separately
- [ ] `aimee delegate execute "..."` output includes token count and cost
- [ ] Dashboard delegations table shows cost per delegation
- [ ] Unknown models fall back to a default pricing tier with a warning

## Owner and Effort

- **Owner:** aimee maintainer
- **Effort:** S (SSE response fields already exist; just need extraction and arithmetic)
- **Dependencies:** Slash commands proposal (for `/cost` command; can also print on session exit)

## Rollout and Rollback

- **Rollout:** Enabled by default. Cost display is informational only.
- **Rollback:** Remove extraction code; leave accumulator structs in place.
- **Blast radius:** Display-only — no behavioral changes.

## Test Plan

- [ ] Unit tests: `estimate_cost_usd()` with known token counts and models
- [ ] Unit tests: accumulator correctly sums across turns
- [ ] Integration tests: mock SSE response with usage fields; verify extraction
- [ ] Manual verification: run multi-turn chat, compare `/cost` output to provider dashboard

## Operational Impact

- **Metrics:** `session.total_tokens`, `session.estimated_cost_usd` (useful for monitoring spend)
- **Logging:** INFO: cost summary at session end
- **Alerts:** Optional: warn when session cost exceeds configurable threshold
- **Disk/CPU/Memory:** Negligible — two structs of integers per session

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Token tracking | P1 | S | High — cost visibility is essential for budget-conscious users |

## Trade-offs

- **Live pricing API** vs **embedded table**: A live API would always be current but adds a network dependency and failure mode. Embedded pricing is stale but reliable. Recommend embedded with periodic manual updates.
- **Per-delegation cost** vs **per-session only**: Per-delegation is more useful (compare cost across roles/providers) but requires changes to delegation recording. Worth the extra effort.
