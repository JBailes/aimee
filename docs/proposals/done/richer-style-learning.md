# Proposal: Richer Style Learning

## Problem

Style learning in `memory_advanced.c:650-714` detects only two dimensions — verbose vs. brief — using hardcoded keyword lists (`style_keywords_negative[]` and `style_keywords_positive[]`). It scans negative feedback rules for these keywords and generates at most 2 preference memories (`output_style` and `output_brevity`).

Real user style preferences are richer: commit message conventions, naming preferences, code comment style, explanation depth, response structure. These are observable from feedback rules and decision outcomes but aren't captured.

Evidence: The function is 64 lines, produces at most 2 memories, and checks against ~15 hardcoded keywords. The feedback rules table contains richer signal that goes unused.

## Goals

- Style learning covers more dimensions beyond verbosity.
- Style preferences are derived from actual feedback patterns, not just keyword lists.
- New style dimensions are extensible without code changes.

## Approach

### 1. Style Dimension Framework

Define style dimensions as keyword→preference mappings in a data table rather than hardcoded arrays:

```c
typedef struct {
    const char *dimension;      /* e.g., "commit_style", "naming", "explanations" */
    const char *keywords[16];   /* trigger words in feedback */
    const char *preference;     /* generated preference text */
    double confidence;
} style_dimension_t;
```

Initial dimensions:

| Dimension | Negative Keywords | Positive Keywords | Preference |
|-----------|------------------|-------------------|------------|
| verbosity | verbose, wordy, lengthy, chatty | concise, brief, terse, short | Prefers concise output |
| explanations | obvious, unnecessary, over-explain | explain, why, reasoning, context | Prefers/avoids detailed explanations |
| commit_style | vague commit, bad message | conventional, semantic | Follows conventional commit style |
| naming | inconsistent, wrong case, naming | snake_case, camelCase, consistent | Prefers specific naming convention |
| comments | too many comments, obvious comments | document, comment, annotate | Prefers minimal/thorough comments |
| structure | wall of text, no headers, unstructured | headers, sections, bullet | Prefers structured output with sections |

### 2. Scan Both Positive and Negative Rules

Current implementation only scans `polarity = 'negative'`. Positive feedback ("perfect, keep doing that") also carries style signal. Scan both polarities:

```sql
SELECT polarity, description FROM rules
WHERE polarity IN ('positive', 'negative')
```

For positive rules, match against positive keywords. For negative rules, match against negative keywords. Weight: positive match = 1.0, negative match = 0.8 (corrections are slightly less confident than confirmations because they might be one-time preferences).

### 3. Decision-Derived Style

Scan `decision_log` for style-relevant decisions:

```sql
SELECT chosen, rationale FROM decision_log
WHERE outcome = 'success'
  AND (chosen LIKE '%style%' OR chosen LIKE '%format%'
       OR chosen LIKE '%naming%' OR chosen LIKE '%convention%')
```

Successful decisions about formatting/style choices become high-confidence preferences.

### 4. Generated Memories

For each dimension with 2+ matches (same threshold as current), generate an L1 preference memory:

```
Key: style_{dimension}
Content: "{preference}. Evidence: {N} feedback signals."
Confidence: 0.8 (negative) or 0.9 (positive confirmed)
```

### Changes

| File | Change |
|------|--------|
| `src/memory_advanced.c` | Replace hardcoded arrays with `style_dimension_t` table, scan both polarities, add decision scanning |
| `src/headers/aimee.h` | Declare `style_dimension_t`, dimension count |

## Acceptance Criteria

- [ ] Style learning detects 6+ dimensions (not just verbosity)
- [ ] Both positive and negative rules contribute to style signals
- [ ] Decision log outcomes feed into style preferences
- [ ] `aimee memory style` shows all detected style preferences
- [ ] Adding a new dimension requires only adding an entry to the data table

## Owner and Effort

- **Owner:** TBD
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Replaces `memory_learn_style()` implementation. New preferences generated on next call (session hook).
- **Rollback:** Revert commit. Existing style memories remain. Reverts to 2-dimension detection.
- **Blast radius:** May generate more L1 preference memories than before (up to 6 vs. 2). Each is small. No impact on other systems.

## Test Plan

- [ ] Unit test: negative rule "too verbose" triggers verbosity dimension
- [ ] Unit test: positive rule "good commit messages" triggers commit_style dimension
- [ ] Unit test: successful decision about naming triggers naming dimension
- [ ] Unit test: 1 match is insufficient (below threshold of 2)
- [ ] Integration test: add feedback rules, run `memory_learn_style()`, verify preferences
- [ ] Manual: `aimee memory style` shows detected dimensions

## Operational Impact

- **Metrics:** None new.
- **Logging:** Detected dimensions logged at debug level.
- **Alerts:** None.
- **Disk/CPU/Memory:** Up to 6 L1 preference memories (vs. 2). Negligible.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Style dimension framework | P3 | S | Extensible replacement |
| Both-polarity scanning | P3 | S | More signal |
| Decision-derived style | P3 | S | Richer inference |

## Trade-offs

**Why data-driven dimensions instead of ML-based style extraction?** The feedback corpus is small (tens of rules, not thousands). Statistical NLP would overfit. Keyword matching is transparent, debuggable, and sufficient for the volume. The framework is extensible — adding dimensions is one struct entry.

**Why keep the 2-match threshold?** A single feedback instance could be situational ("this commit message was too vague" doesn't mean the user always wants detailed commit messages). Two instances suggest a pattern. The threshold could be made per-dimension later.
