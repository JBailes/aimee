# Proposal: Tool Result Compaction

## Problem

When aimee's MCP tools return large outputs — full file contents, verbose shell output, large JSON API responses — the entire result is passed into the agent's context window. This wastes tokens and can push conversations toward context limits faster than necessary.

Currently there is no per-tool-result size management. The session-compaction proposal (pending) addresses whole-session compaction, but that's a blunt instrument — by the time session compaction triggers, token budget has already been wasted on oversized individual tool results that could have been reduced at insertion time.

ayder-cli implements a `truncate_tool_result` function (`core/context_manager.py`) that:
- Detects JSON tool results and replaces them with a structural summary (key names, array lengths, sample items)
- For plain text, keeps the first 40% and last 10% of the content with a truncation notice in between
- Applies a configurable `max_chars` threshold (default 8192)

This is a simple, high-leverage optimization that reduces per-turn token waste without losing critical information.

Evidence:
- `mcp_tools.c` passes tool results directly to the agent with no size management
- Large `git diff`, `grep`, or file-read results can easily be 10-50KB, consuming 2500-12500 tokens each
- Delegates with smaller context windows (ollama models) are especially sensitive to oversized tool results

## Goals

- Tool results exceeding a configurable size threshold are compacted before being sent to the agent context.
- JSON results are replaced with a structural summary preserving key names, array sizes, and sample values.
- Plain-text results keep head and tail content with a truncation notice.
- The original untruncated result is preserved in the session log for debugging.
- Compaction is configurable per-tool or globally, with sensible defaults.

## Approach

### 1. Compaction function

Add `tool_result_compact()` to a new `src/compact.c` (or extend existing context management):

```c
typedef struct {
    int max_chars;         // default 8192
    float head_ratio;      // default 0.4
    float tail_ratio;      // default 0.1
} compact_opts_t;

// Returns a newly allocated compacted string, or NULL if no compaction needed.
// Caller frees.
char *tool_result_compact(const char *result, int result_len, const compact_opts_t *opts);
```

Logic:
1. If `result_len <= opts->max_chars`, return NULL (no compaction).
2. If result starts with `{` or `[`, attempt JSON structural summary.
3. Otherwise, extract head (40%) + truncation notice + tail (10%).

### 2. JSON structural summary

```c
// Recursively summarize JSON structure up to max_depth=2
// Example output: "{users: [150 items, e.g.: {id: "abc...", name: "Jo..."}], total: 150}"
char *json_structure_summary(const char *json, int max_depth);
```

### 3. Integration point

In `mcp_tools.c`, after tool execution and before result is added to the conversation context, apply compaction:

```c
char *compacted = tool_result_compact(result, strlen(result), &global_compact_opts);
// Store original in session log
session_log_tool_result(session, tool_name, result);
// Use compacted version (or original if NULL) for context
const char *ctx_result = compacted ? compacted : result;
```

### 4. Per-tool overrides

Some tools (e.g., `read_file`) may want different thresholds than others (e.g., `git_diff`). Support per-tool config:

```
[compact]
default_max_chars = 8192
[compact.overrides]
git_diff = 16384
read_file = 12288
```

### Changes

| File | Change |
|------|--------|
| `src/compact.c` (new) | `tool_result_compact()`, `json_structure_summary()`, plain-text head/tail truncation |
| `src/headers/compact.h` (new) | Public API for compaction |
| `src/mcp_tools.c` | Apply compaction to tool results before adding to context |
| `src/config.c` | Parse `[compact]` config section |
| `tests/test_compact.c` (new) | Unit tests for compaction logic |

## Acceptance Criteria

- [ ] Tool results >8192 chars are compacted by default
- [ ] JSON results produce a structural summary with key names, array lengths, and sample values
- [ ] Plain-text results show head (40%) + truncation notice + tail (10%)
- [ ] Original uncompacted results are preserved in session log
- [ ] Per-tool threshold overrides work via config
- [ ] Compaction can be disabled globally with `default_max_chars = 0`

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** M (2-3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Enabled by default with 8192 char threshold. Existing sessions see compacted results on next tool call.
- **Rollback:** Set `default_max_chars = 0` in config to disable. Revert commit for full removal.
- **Blast radius:** Only affects tool result display in agent context. Original results preserved in logs. No data loss risk.

## Test Plan

- [ ] Unit tests: JSON compaction (nested objects, arrays, empty, malformed), plain-text compaction (short, exact threshold, large), threshold=0 disabling
- [ ] Integration tests: MCP tool call with large output, verify compacted result in context and original in log
- [ ] Failure injection: Invalid JSON (falls through to plain-text compaction), empty result, result exactly at threshold
- [ ] Manual verification: Run `aimee delegate code "read a large file"` and verify context shows compacted output

## Operational Impact

- **Metrics:** `tool_results_compacted_total`, `tool_result_bytes_saved_total`
- **Logging:** Compaction events at DEBUG, compaction failures at WARN
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — JSON parsing is bounded by max_chars, no extra allocations for results under threshold

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Core compaction function | P2 | S | High — immediate token savings |
| JSON structural summary | P2 | M | High — preserves structure for API results |
| Per-tool overrides | P3 | S | Low — fine-tuning |
| Metrics | P3 | S | Low — observability |

## Trade-offs

- **Why not just use session compaction?** Session compaction is reactive — it triggers when the whole session is large. Tool result compaction is proactive — it prevents bloat at the source. Both are complementary.
- **Why head/tail instead of just head?** The tail often contains summary information, return values, or error messages. Keeping the tail preserves diagnostic value.
- **Why 8192 default?** Roughly 2048 tokens. Large enough to capture most useful tool output, small enough to prevent a single tool call from consuming a significant fraction of a 32K context window.

## Source Reference

Implementation reference: ayder-cli `src/ayder_cli/core/context_manager.py` — `truncate_tool_result()` and `_summarize_structure()` functions.
