# Proposal: Dynamic Tool Output Truncation

## Problem

Large tool outputs (multi-thousand-line grep results, huge file reads, verbose bash output) consume disproportionate context window space. A single large `grep` result can fill 30-50% of a delegate's context, leaving insufficient room for reasoning and additional tool calls. This causes premature compaction and degraded output quality.

Evidence: oh-my-openagent implements a `dynamic-truncator` (`src/shared/dynamic-truncator.ts`) that estimates token count of tool outputs and truncates to a budget. It preserves header lines, cuts from the end, and appends `[TRUNCATED]`. The budget adjusts dynamically based on remaining context window capacity.

## Goals

- Cap tool output at a configurable token budget (default: 50K tokens)
- Preserve the first N lines (headers/context) when truncating
- Append clear `[TRUNCATED — N lines removed]` indicator
- Dynamic budget: reduce the cap as context usage increases

## Approach

After each tool call returns, estimate the output's token count (chars / 4). If it exceeds the budget, truncate from the end while preserving the first N lines. Append a truncation indicator.

### Token estimation

```c
int estimate_tokens(const char *text, int len) {
    return (len + 3) / 4;  // ~4 chars per token
}
```

### Truncation strategy

1. Preserve first 3 lines (headers)
2. Keep content from the end up to the budget
3. Append `[Output truncated: removed N lines to fit context budget]`

### Changes

| File | Change |
|------|--------|
| `src/mcp_tools.c` | Add `truncate_tool_output()` post-processing for all tool results |
| `src/config.c` | Parse tool output token budget from project.yaml |

## Acceptance Criteria

- [ ] Tool output >50K tokens is truncated
- [ ] First 3 lines are always preserved
- [ ] Truncation indicator shows how many lines were removed
- [ ] Tool output ≤50K tokens passes through unchanged
- [ ] Budget is configurable via project.yaml

## Owner and Effort

- **Owner:** aimee
- **Effort:** S–M (1–2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; default budget of 50K tokens
- **Rollback:** Remove truncation; full tool output as before
- **Blast radius:** Affects large tool outputs only; small outputs unchanged

## Test Plan

- [ ] Unit test: output under budget passes unchanged
- [ ] Unit test: output over budget is truncated with indicator
- [ ] Unit test: first 3 lines are preserved
- [ ] Unit test: truncation indicator shows correct removed line count
- [ ] Integration test: large grep result is truncated to budget

## Operational Impact

- **Metrics:** Truncation events per session, bytes removed per truncation
- **Logging:** Log truncation at debug level with original and truncated sizes
- **Disk/CPU/Memory:** Negligible — one pass over output string

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Dynamic Tool Output Truncation | P2 | S–M | Medium — prevents context exhaustion from large outputs |

## Trade-offs

Alternative: let the agent handle large outputs by asking it to re-query with filters. Unreliable — agents often don't notice the output is too large until context is already consumed.

Alternative: fixed truncation per tool type. Less flexible — a 50K budget is fine for most tools but too generous for some (e.g., recursive grep). Dynamic per-session adjustment is better.

Inspiration: oh-my-openagent `src/shared/dynamic-truncator.ts`
