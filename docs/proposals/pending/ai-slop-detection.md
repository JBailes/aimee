# Proposal: AI Slop Detection and Cleanup in Guardrails

## Problem

When delegates (or the primary agent) generate code, it often contains "AI slop" — patterns that work but degrade codebase quality:
- Dead code and unreachable branches
- Needless abstractions (pass-through wrappers, single-use helpers)
- Copy-pasted logic with minor variations
- Speculative error handling for impossible cases
- Verbose comments restating the obvious

Aimee's guardrails (`guardrails.c`) currently check for sensitive files, anti-patterns in tool usage, and plan mode enforcement. They do not inspect the *content* of generated code for quality issues.

oh-my-codex's `$ai-slop-cleaner` skill addresses this with a systematic 5-category detection pass (duplication, dead code, needless abstraction, boundary violations, missing tests) that runs after code generation, with regression tests locked before cleanup. The key value is catching AI-generated quality debt before it's committed.

Evidence:
- `guardrails.c` has `post_tool_update()` that fires on every PostToolUse — perfect hook point
- `trace_analysis.c` already mines traces for patterns but focuses on execution anti-patterns, not code quality
- No existing mechanism inspects file contents after writes for quality issues
- The existing `aimee verify` runs build/test but not code quality analysis

## Goals

- Code written by delegates or the primary agent is automatically checked for common AI slop patterns
- Detection runs as a PostToolUse check after file writes, not as a separate manual step
- Findings are surfaced in the session context so the agent can self-correct
- A dedicated `aimee clean` command runs targeted cleanup on specified files
- Detection is lightweight — pattern matching in C, not delegate calls (save expensive delegate review for structured-code-review proposal)

## Approach

### 1. Pattern-based slop detection in C

Add `slop_detect.c` with fast pattern matching for common AI slop:

```c
typedef enum {
    SLOP_DEAD_CODE,       // unreachable after return/exit, #if 0 blocks
    SLOP_PASS_THROUGH,    // functions that just call another function with same args
    SLOP_DUPLICATE,       // identical or near-identical blocks within a file
    SLOP_VERBOSE_COMMENT, // comments restating the line below ("increment i" above "i++")
    SLOP_SPECULATIVE,     // catch blocks for errors that can't happen in context
} slop_category_t;

typedef struct {
    slop_category_t category;
    int line;
    int severity;  // 0=info, 1=warn, 2=error
    char description[256];
} slop_finding_t;

int slop_detect_file(const char *path, slop_finding_t *out, int max_findings);
```

Detection heuristics (fast, line-based, no AST):
- **Dead code**: code after unconditional `return`/`exit`/`break` in same block
- **Pass-through**: function body is a single `return other_func(same, args);`
- **Duplicate**: sliding window comparison of 5+ line blocks within the same file (normalized whitespace)
- **Verbose comments**: single-line comment immediately above a line, where the comment adds no information beyond the code
- **Speculative**: empty catch/except blocks, `if (ptr == NULL)` right after a guaranteed-non-null allocation

### 2. PostToolUse integration

In `post_tool_update()`, after a file write (Edit or Write tool), run `slop_detect_file()` on the modified file. If findings exist, append them to the session context:

```
# Code Quality Notes
- src/foo.c:42 [dead_code] Unreachable code after return on line 41
- src/foo.c:78 [pass_through] Function bar() is a pass-through to baz()
```

This is advisory — it doesn't block the tool call, but the primary agent sees the notes and can self-correct.

### 3. `aimee clean` CLI command

For explicit cleanup runs:

```bash
aimee clean <file>              # detect slop in a specific file
aimee clean --changed           # detect in all files changed since last commit
aimee clean --fix <file>        # delegate cleanup to a code delegate
```

`--fix` delegates the cleanup to a `refactor` role delegate with the findings as context, then re-runs detection to verify the fixes didn't introduce regressions.

### 4. Working memory integration

Store slop findings in working memory so they persist within the session:

```c
wm_set(db, session_id, "slop:src/foo.c", findings_json, "code_quality", 3600);
```

TTL of 1 hour — findings are relevant for the current editing session, not permanently.

### Changes

| File | Change |
|------|--------|
| `src/slop_detect.c` | New: pattern-based slop detection heuristics |
| `src/headers/slop_detect.h` | New: slop types and detection function |
| `src/guardrails.c` | Extend `post_tool_update()` to run slop detection on file writes |
| `src/cmd_core.c` | Add `clean` subcommand |
| `src/tests/test_slop_detect.c` | Tests with known-slop test files |

## Acceptance Criteria

- [ ] `slop_detect_file()` detects dead code, pass-throughs, duplicates, verbose comments, and speculative handling
- [ ] PostToolUse on file writes runs detection and surfaces findings in session context
- [ ] `aimee clean <file>` reports findings with file:line references
- [ ] `aimee clean --fix <file>` delegates cleanup and verifies
- [ ] Detection completes in <50ms per file (no delegate calls, pure C pattern matching)
- [ ] False positive rate is acceptable — detection is conservative (prefer false negatives)

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (3-4 focused sessions)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** PostToolUse detection is advisory only — no blocking behavior. Ship detection first, `--fix` second.
- **Rollback:** Revert commit. No DB changes. Guardrails revert to previous behavior.
- **Blast radius:** None. Advisory only. Worst case: slightly more text in session context.

## Test Plan

- [ ] Unit tests: each slop category with known-good and known-bad files
- [ ] Unit tests: false positive check — clean files produce no findings
- [ ] Unit tests: performance — detection on 1000-line files completes in <50ms
- [ ] Integration tests: write a file with slop → PostToolUse produces findings
- [ ] Manual verification: introduce dead code, observe it flagged in session context

## Operational Impact

- **Metrics:** `slop_findings_total{category}`, `slop_detect_latency_ms`
- **Logging:** Detection results to stderr: `aimee: slop: src/foo.c: 2 findings (1 dead_code, 1 pass_through)`
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible. Pattern matching is O(n) per file, no I/O beyond reading the file.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Core detection heuristics | P1 | M | High — catches the most common issues |
| PostToolUse integration | P1 | S | High — makes detection automatic |
| CLI `clean` command | P2 | S | Medium — manual trigger |
| `--fix` delegate cleanup | P3 | S | Low — nice-to-have, manual cleanup works |

## Trade-offs

**Why C pattern matching instead of delegate-based analysis?**
Speed. PostToolUse runs on every file write. A delegate call per write would add seconds of latency and cost tokens. Pattern matching in C is effectively free. Save delegate-based review for the structured-code-review proposal which runs on the full diff, not per-write.

**Why conservative (prefer false negatives)?**
False positives in session context would train the primary agent to ignore findings. Better to catch 60% of real slop with high confidence than 90% with frequent false alarms.

**Why not integrate with existing trace_analysis.c?**
Trace analysis mines *execution* patterns (retry loops, tool sequences). Slop detection inspects *code* content. Different inputs, different heuristics, different output formats. Keeping them separate is cleaner.
