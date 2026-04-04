# Proposal: Token Usage, Cost, and Context Budget Tracking

## Problem

The pending set currently has four overlapping proposals for token tracking:

- raw token usage display
- per-model cost estimation
- cache-aware accounting
- context-budget utilization

These are not separate features in practice. The data pipeline is shared: parse provider usage, normalize it, persist it per turn/session, then expose it in CLI, webchat, dashboards, and delegate stats. Splitting them creates duplicated structs, inconsistent pricing rules, and competing UI surfaces.

## Goals

- Track per-turn and cumulative usage for all supported execution surfaces.
- Support the four useful categories: input, output, cache write, cache read.
- Estimate USD cost from model-specific pricing tables.
- Show context-window utilization and budget pressure.
- Persist usage into session/delegation records for dashboards, resume, and audits.
- Provide sensible fallback estimation when providers omit usage metadata.

## Approach

Introduce one shared usage subsystem that normalizes provider responses and powers all displays.

### Usage Model

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
    int64_t estimated_context_used;
    int64_t context_window_size;
    double estimated_cost_usd;
} usage_tracker_t;
```

### Shared Pipeline

1. Extract usage from OpenAI/Anthropic responses and SSE end states.
2. Normalize into the shared `token_usage_t`.
3. Estimate missing data when the provider does not report usage.
4. Calculate:
   - total usage
   - cache hit rate
   - context utilization percentage
   - per-model and cumulative estimated cost
5. Surface the same data consistently in chat, webchat, delegates, dashboard, and command output.

### Display Surfaces

- CLI post-turn status and `/cost` or `/status`
- webchat status bar and per-session detail
- delegation summaries and attempt logs
- dashboard cost/token columns and aggregates

### Changes

| File | Change |
|------|--------|
| `src/token_tracker.c` | New shared token, utilization, cache-rate, and cost logic |
| `src/headers/token_tracker.h` | Public tracker/pricing API |
| `src/cmd_chat.c` | Extract usage and display session stats in CLI chat |
| `src/webchat.c` | Extract usage and emit usage updates to the browser |
| `src/webchat_assets.c` | Show token, context, and cost status |
| `src/agent_coord.c` | Record usage and cost for delegates |
| `src/dashboard.c` | Add usage/cost aggregates and per-session visibility |
| `src/config.c` | Parse model context sizes and optional budget thresholds |

## Acceptance Criteria

- [ ] OpenAI and Anthropic usage responses normalize into one internal format.
- [ ] All four categories are tracked when available.
- [ ] Cost estimation uses model-specific pricing and cache pricing where applicable.
- [ ] CLI and webchat both show cumulative usage and context utilization.
- [ ] Delegation stats include token usage and estimated cost.
- [ ] Unknown or non-reporting providers fall back to explicit estimation with a flag.
- [ ] Budget thresholds can warn or stop execution when configured.

## Owner and Effort

- **Owner:** aimee maintainer
- **Effort:** M (2-3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Tracking is always-on; hard budget enforcement remains opt-in.
- **Rollback:** Remove displays and tracker plumbing. No core execution behavior depends on it.
- **Blast radius:** Mispriced models can mislead users, so displays must stay labeled as estimates.

## Test Plan

- [ ] Unit tests for normalization, accumulation, cache hit rate, cost calculation, and utilization.
- [ ] Integration tests with mocked OpenAI and Anthropic responses.
- [ ] Integration tests for provider-missing-usage fallback.
- [ ] Manual verification against provider dashboards for a sample session.

## Operational Impact

- **Metrics:** `session_tokens_total`, `session_estimated_cost_usd`, `token_estimation_fallback_total`
- **Logging:** DEBUG per request, INFO summaries at session/delegate end
- **Alerts:** Optional warning on budget threshold crossings
- **Disk/CPU/Memory:** Negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Shared usage normalization | P1 | S | High |
| CLI/webchat visibility | P1 | S | High |
| Delegate/dashboard persistence | P2 | S | High |
| Budget enforcement | P3 | S | Medium |

## Trade-offs

- **Why merge all token proposals?** Separate proposals would duplicate the same plumbing and drift on definitions.
- **Why include context utilization here?** Budget pressure is part of usage observability, not a separate subsystem.
- **Why keep estimation fallback?** Approximate visibility is more useful than silence for non-reporting providers.
