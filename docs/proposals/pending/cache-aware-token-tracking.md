# Proposal: Cache-Aware Token Tracking (4-Category)

## Problem

Aimee's token usage tracking does not distinguish between cache hits and cache misses. Anthropic's API returns four distinct token categories:

- `input_tokens` — fresh input tokens (full price)
- `output_tokens` — generated output tokens
- `cache_creation_input_tokens` — tokens written to cache (1.25x price)
- `cache_read_input_tokens` — tokens read from cache (0.1x price)

Without tracking these separately, cost estimation is inaccurate — cache reads are 90% cheaper than fresh input, so treating all input tokens equally significantly overestimates cost.

This matters for cost display in both CLI and webchat (see: token-cost-estimation proposal).

The `soongenwong/claudecode` repo at `rust/crates/api/src/types.rs` and `rust/crates/runtime/src/usage.rs` tracks all four categories.

## Goals

- Token usage tracks 4 categories: input, output, cache_creation, cache_read.
- Cost estimation accounts for cache pricing tiers.
- CLI and webchat status displays show cache hit rate alongside token counts.
- Session persistence preserves per-category usage data.

## Approach

### Usage Structure

```c
typedef struct {
    uint32_t input_tokens;
    uint32_t output_tokens;
    uint32_t cache_creation_input_tokens;
    uint32_t cache_read_input_tokens;
} token_usage_t;

uint32_t usage_total_tokens(const token_usage_t *u) {
    return u->input_tokens + u->output_tokens
         + u->cache_creation_input_tokens + u->cache_read_input_tokens;
}

double usage_cache_hit_rate(const token_usage_t *u) {
    uint32_t total_input = u->input_tokens + u->cache_read_input_tokens;
    return total_input ? (double)u->cache_read_input_tokens / total_input : 0.0;
}
```

### Display

```
Tokens: 42.1k in (87% cached) / 8.3k out | ~$0.12
```

### Changes

| File | Change |
|------|--------|
| `src/agent_eval.c` | Parse all 4 usage fields from API response |
| `src/server_session.c` | Accumulate per-category usage, persist in session state |
| `src/render.c` | Display cache hit rate in status output |
| `src/webchat.c` | Show cache hit rate in webchat session detail |

## Acceptance Criteria

- [ ] All 4 token categories are parsed from API responses
- [ ] Cost estimation uses correct per-category pricing
- [ ] Cache hit rate is displayed in CLI and webchat
- [ ] Session state persists per-category usage across compaction/resume

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S (half day)
- **Dependencies:** Complements token-cost-estimation proposal

## Rollout and Rollback

- **Rollout:** Always-on — more accurate tracking replaces less accurate.
- **Rollback:** Revert to single input_tokens field.
- **Blast radius:** None.

## Test Plan

- [ ] Unit tests: usage accumulation, cache hit rate calculation, cost with cache tiers
- [ ] Integration tests: parse real API response with all 4 fields

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| 4-category parsing | P2 | S | High — accuracy |
| Cache hit rate display | P3 | S | Medium — user insight |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/api/src/types.rs` (lines 149-164).
