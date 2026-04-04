# Proposal: Tool Result Compaction and Dynamic Truncation

## Problem

There are two overlapping proposals for oversized tool output:

- static or structured tool-result compaction
- dynamic truncation based on remaining context budget

These should be one proposal. Per-result compaction should combine structure-aware summaries with budget-aware truncation rather than picking one technique.

## Goals

- Prevent oversized tool results from consuming disproportionate context.
- Summarize JSON and structured outputs intelligently.
- Truncate plain text in a way that preserves diagnostic value.
- Adjust budgets dynamically as remaining context shrinks.
- Preserve original results in logs/session storage for debugging.

## Approach

Build one tool-result compaction layer applied before results enter model context.

### Strategy

1. If the result is small enough, pass through unchanged.
2. If the result is JSON, emit a structural summary.
3. If the result is plain text, keep relevant head/tail regions plus a truncation notice.
4. Reduce the allowed budget as session context usage rises.

### Configuration

- global default thresholds
- per-tool overrides
- optional disable flag

### Changes

| File | Change |
|------|--------|
| `src/compact.c` | Tool-result compaction and JSON structure summarization |
| `src/headers/compact.h` | Public API |
| `src/mcp_tools.c` | Apply compaction before adding results to context |
| `src/config.c` | Parse compaction budgets and per-tool overrides |
| `tests/test_compact.c` | Unit tests for compaction logic |

## Acceptance Criteria

- [ ] Large tool results are compacted before entering context by default.
- [ ] JSON results produce structural summaries with key names and array/item hints.
- [ ] Plain-text results preserve diagnostically useful head/tail content with explicit truncation notices.
- [ ] Dynamic budget adjustment lowers result limits as context usage rises.
- [ ] Original uncompacted results remain available in logs/session state.
- [ ] Compaction is configurable globally and per tool.

## Owner and Effort

- **Owner:** aimee
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Start with static compaction thresholds, then add dynamic context-aware budgeting.
- **Rollback:** Disable compaction through config and fall back to raw tool results.
- **Blast radius:** Affects how tools appear in context, not the underlying tool execution.

## Test Plan

- [ ] Unit tests: JSON summarization, plain-text truncation, threshold edge cases
- [ ] Unit tests: dynamic budget adjustment
- [ ] Integration tests: large tool outputs compact in context while originals remain logged

## Operational Impact

- **Metrics:** `tool_results_compacted_total`, `tool_result_bytes_saved_total`, `dynamic_truncations_total`
- **Logging:** DEBUG on compaction decisions, WARN on summarization failures
- **Alerts:** None
- **Disk/CPU/Memory:** Small parsing/truncation overhead

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Core compaction function | P1 | S | High |
| JSON structural summary | P1 | S | High |
| Dynamic budget adjustment | P2 | S | Medium |

## Trade-offs

- **Why merge these proposals?** Structured compaction and dynamic truncation solve the same context-budget problem at different levels.
- **Why preserve original results?** Debugging and auditability still need raw output.
- **Why head/tail for plain text?** Errors and summaries often live at opposite ends of command output.
