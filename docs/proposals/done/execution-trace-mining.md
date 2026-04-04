# Proposal: Execution Trace Pattern Mining

## Problem

Every agent turn is logged in the execution_trace table (agent_plan.c records tool_name, tool_args, direction, turn number). This is the richest signal source in the system, but it is write-only: nothing reads it back for learning. Patterns like "tool X always fails before tool Y succeeds" or "3+ retries on the same file indicate wrong approach" or "agent always uses tool A then tool B for this type of task" could be extracted automatically and fed into the rules or memory system.

## Goals

- Extract recurring tool-use patterns from execution traces
- Identify failure patterns (retry loops, repeated errors)
- Surface successful tool sequences as learned procedures
- Feed discovered patterns into rules/memory for future agent guidance

## Approach

Add a `trace_mine()` function in a new `src/trace_analysis.c` file that runs during session cleanup (similar to `eval_feedback_loop`). It reads execution traces, extracts patterns, and writes discoveries into the existing anti-pattern and memory systems.

### Pattern types to extract

1. **Retry loops:** Same tool with similar args called 3+ times in sequence with failures between. Record as anti-pattern.
2. **Recovery sequences:** Tool A fails, then tool B succeeds on the same target. Record as procedure memory (`kind=procedure`).
3. **Common sequences:** Tool A followed by tool B in >60% of traces for the same plan type. Record as procedure memory.

### Schema addition

```sql
CREATE TABLE trace_mining_log (
    id INTEGER PRIMARY KEY,
    last_trace_id INTEGER,
    mined_at TEXT
);
```

### Changes

| File | Change |
|------|--------|
| `src/trace_analysis.c` | New file. Implements `trace_mine()` with retry loop detection, recovery sequence detection, and common sequence extraction |
| `src/headers/trace_analysis.h` | New file. Header for trace_analysis.c |
| `src/cmd_hooks.c` | Call `trace_mine()` at session cleanup |
| `src/db.c` | Add migration for `trace_mining_log` table |

## Acceptance Criteria

- [ ] `trace_mine()` detects retry loops (same tool+similar args called 3+ times with failures) and records them as anti-patterns
- [ ] `trace_mine()` detects recovery sequences (tool A fails, tool B succeeds on same target) and records them as procedure memories
- [ ] `trace_mine()` detects common sequences (tool A then tool B in >60% of traces for same plan type) and records them as procedure memories
- [ ] Mining runs automatically during session cleanup
- [ ] `trace_mining_log` tracks processed traces so mining is idempotent (reprocessing the same session produces no duplicates)
- [ ] Mining completes in <100ms for typical sessions (~50 trace entries)

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (medium)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Automatic via schema migration and session cleanup hook. No feature flag needed.
- **Rollback:** Revert commit. The `trace_mining_log` table and any mined data are purely additive; removing them has no side effects.
- **Blast radius:** Low. Mining is read-only on execution traces, write-only on memories and anti-patterns. A bug in mining cannot corrupt existing data.

## Test Plan

- [ ] Unit tests: Retry loop detection (given traces with 3+ repeated tool calls and failures, verify anti-pattern creation)
- [ ] Unit tests: Recovery sequence detection (given tool A failure followed by tool B success, verify procedure memory creation)
- [ ] Integration tests: Create traces with known patterns, run `trace_mine()`, verify correct outputs in anti-pattern and memory tables
- [ ] Integration tests: Verify idempotency by running `trace_mine()` twice on the same data and confirming no duplicate entries
- [ ] Failure injection: Empty trace table, single-entry trace, trace with no discernible patterns

## Operational Impact

- **Metrics:** None added (could add mining duration as a future enhancement).
- **Logging:** `trace_mine()` logs count of patterns discovered per run.
- **Alerts:** None.
- **Disk/CPU/Memory:** Mining runs once per session cleanup. For typical sessions (~50 trace entries), completes in <100ms. `trace_mining_log` adds 1 row per mining run.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Execution trace pattern mining | P2 | M | Enables automatic learning from agent behavior, improving future sessions |

## Trade-offs

**Why not mine in real-time?** Trace mining requires looking at sequences across multiple turns, which is inherently retrospective. Real-time detection of retry loops is better handled by in-session anti-pattern detection (a separate concern).

**Why a 60% threshold for common sequences?** Lower thresholds produce too many spurious patterns. Higher thresholds miss useful but not universal sequences. 60% balances signal and noise; this can be tuned after observing real data.

**Why not use the agent itself to analyze traces?** Keeping mining in C as a deterministic function avoids LLM costs, latency, and non-determinism. The patterns we extract are structural (sequence matching, counting), not semantic.
