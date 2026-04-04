# Proposal: Token Usage Tracking with Per-Model Cost Estimation

## Problem

Aimee tracks token counts but provides no cost visibility. Users running long delegate sessions, multiple agents, or expensive models have no way to see session cost in USD, compare providers, or set budget limits. This matters for both CLI and webchat — webchat users especially need cost visibility for longer, exploratory sessions.

The `soongenwong/claudecode` repo at `rust/crates/runtime/src/usage.rs` implements per-turn and cumulative token accumulation with per-model pricing tables and USD cost calculation.

## Goals

- Every session (CLI and webchat) displays cumulative token counts and estimated USD cost.
- Cost is computed per-model using a pricing table covering major providers.
- Webchat dashboard shows per-session and per-agent cost breakdowns.
- Optional budget limit stops agent execution when cost exceeds threshold.

## Approach

### Pricing Table

```c
static const struct model_pricing PRICING[] = {
    {"claude-opus",    15.00, 75.00,  1.50,  18.75},
    {"claude-sonnet",  3.00,  15.00,  0.30,   3.75},
    {"claude-haiku",   0.25,   1.25,  0.03,   0.30},
    {"gpt-4o",         2.50,  10.00,  0.00,   0.00},
    {"grok-3",         3.00,  15.00,  0.00,   0.00},
    {NULL, 0, 0, 0, 0}
};
```

### Display Surfaces

| Surface | Display |
|---------|---------|
| CLI status line | `[tokens: 42.1k in / 8.3k out | ~$0.47]` |
| `/cost` command | Detailed breakdown by model, cache stats, total |
| Webchat dashboard | Per-session cost column, per-agent cost in session detail |
| Webchat session header | Running cost counter |

### Changes

| File | Change |
|------|--------|
| `src/cost_tracker.c` (new) | Pricing table, per-model cost calculation, cumulative tracking, budget checking |
| `src/headers/cost_tracker.h` (new) | Public API |
| `src/server_session.c` | Accumulate usage from API responses |
| `src/mcp_tools.c` | Add `session_cost` tool |
| `src/cmd_core.c` | Add `/cost` slash command |
| `src/webchat.c` | Render cost in session list and detail views |
| `src/dashboard.c` | Add cost column to session table |

## Acceptance Criteria

- [ ] CLI sessions show token count and estimated cost in status output
- [ ] `/cost` shows detailed breakdown: tokens by type, per-model cost, total
- [ ] Webchat session list and detail show cost
- [ ] Budget limit stops execution with clear message when exceeded
- [ ] Pricing table covers Anthropic, OpenAI, and xAI models

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S (1-2 days)
- **Dependencies:** Benefits from multi-provider-model-aliasing proposal

## Rollout and Rollback

- **Rollout:** Cost display is always-on (informational). Budget limit is opt-in.
- **Rollback:** Remove cost display — no functional impact.
- **Blast radius:** Incorrect pricing → misleading display. Labeled "estimated" to set expectations.

## Test Plan

- [ ] Unit tests: cost calculation per model, cache discount handling, budget threshold
- [ ] Integration tests: track usage across multi-turn session
- [ ] Manual verification: compare displayed cost against API dashboard

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Pricing table + cost calc | P2 | S | High |
| CLI + webchat cost display | P2 | S | High |
| Budget limit | P3 | S | Medium |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/runtime/src/usage.rs`.
