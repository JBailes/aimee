# Proposal: Performance SLO Dashboard and Regression Gates

## Problem

There are no automated guardrails to detect performance regressions in core
operations. A change that adds 50ms to `memory.search` latency or doubles
startup time would only be noticed through manual observation. Key paths:

- Startup latency (CLI → server connection → ready)
- `memory.search` response time
- Hook execution overhead (`pre_tool_check`)
- Delegation dispatch latency
- Database open + migration time

## Goals

- Define SLO thresholds for p50/p95 of key operations.
- CI blocks PRs that regress beyond threshold.
- Dashboard displays latency trends for operator visibility.

## Approach

### 1. Benchmark suite

Create a repeatable benchmark suite in `tests/bench/`:

```c
/* bench_memory_search.c */
void bench_memory_search(bench_ctx_t *ctx) {
    /* Setup: insert 1000 memories across tiers */
    /* Benchmark: 100 searches with varying query complexity */
    /* Report: p50, p95, p99 */
}
```

Benchmarks:
| Operation | Target p50 | Target p95 | Tolerance |
|-----------|-----------|-----------|-----------|
| CLI startup (cold) | <100ms | <300ms | +20% |
| CLI startup (warm, server running) | <10ms | <30ms | +20% |
| `memory.search` (1000 memories) | <5ms | <20ms | +30% |
| `pre_tool_check` | <1ms | <3ms | +50% |
| `delegate` dispatch | <10ms | <30ms | +20% |
| `db_open` | <20ms | <50ms | +20% |

### 2. Baseline storage

Store baseline measurements in `tests/bench/baseline.json`:
```json
{
    "version": "0.9.3",
    "timestamp": "2026-04-01T00:00:00Z",
    "results": {
        "memory_search": {"p50_ms": 3.2, "p95_ms": 12.1, "p99_ms": 18.5},
        "startup_cold": {"p50_ms": 72, "p95_ms": 195, "p99_ms": 280}
    }
}
```

### 3. CI regression gate

Add a `make bench-check` target that:
1. Runs benchmark suite
2. Compares against baseline
3. Fails if any metric exceeds tolerance
4. Outputs human-readable comparison table

Run on PRs that touch `src/` files. Skip for docs-only PRs.

### 4. Dashboard integration

Add performance panel to `aimee dashboard` showing:
- Last benchmark results
- Trend over recent builds (if historical data available)
- SLO status (pass/fail per metric)

### Changes

| File | Change |
|------|--------|
| `src/tests/bench/` | New benchmark suite (memory_search, startup, pre_tool_check, delegate, db_open) |
| `tests/bench/baseline.json` | Baseline measurements |
| `src/Makefile` | Add `bench` and `bench-check` targets |
| `src/dashboard.c` | Add performance panel |

## Acceptance Criteria

- [ ] Benchmark suite covers all key operations with deterministic setup
- [ ] `make bench` produces p50/p95/p99 measurements
- [ ] `make bench-check` fails when any metric exceeds tolerance over baseline
- [ ] Baseline can be updated with `make bench-baseline`
- [ ] Dashboard shows latest benchmark results

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Benchmark suite ships as test artifact. CI gate added as non-blocking initially, then blocking after baseline is validated.
- **Rollback:** Remove CI gate. Benchmark suite remains as manual tool.
- **Blast radius:** False positives from noisy CI environments could block PRs. Tolerance bands should account for CI variance.

## Test Plan

- [ ] Unit test: benchmark harness correctly computes percentiles
- [ ] Unit test: regression detection triggers at correct threshold
- [ ] Integration test: `make bench` completes and produces valid JSON output
- [ ] Manual: verify dashboard performance panel renders correctly

## Operational Impact

- **Metrics:** Benchmark results stored as CI artifacts.
- **Logging:** None at runtime.
- **Alerts:** CI failure on regression.
- **Disk/CPU/Memory:** Benchmark run adds ~30-60s to CI. No runtime impact.

## Priority

P2 — prevents gradual latency regressions.

## Trade-offs

**Why percentage tolerance instead of absolute thresholds?** Absolute thresholds
break when CI hardware changes. Percentage tolerance is relative to the baseline,
which is re-measured on the same hardware.

**Why not continuous profiling?** aimee is a local tool with no production traffic
to profile. Deterministic benchmarks are the right methodology for this context.
