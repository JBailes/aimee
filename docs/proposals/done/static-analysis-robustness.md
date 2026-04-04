# Proposal: Robust Static Analysis Gate

## Problem

`make static-analysis` (which runs cppcheck and clang-tidy) fails hard when the required tools are absent. This causes contributor workflows to fail inconsistently by environment with no clear diagnostic. There is no distinction between "tool missing" (environment issue) and "analysis found errors" (code issue).

Additionally, the Makefile has no mechanism to control strictness level between local development (where missing tools should warn, not block) and CI (where missing tools should fail).

## Goals

- `make static-analysis` provides clear, actionable output in both local and CI environments.
- Missing tools produce a diagnostic message, not a cryptic build failure.
- CI enforces strict mode; local defaults to non-strict.

## Approach

### 1. Preflight tool checks

Before running cppcheck or clang-tidy, check that the binary exists. If missing:
- Print a clear message: `"cppcheck not found. Install with: apt install cppcheck"`
- In strict mode (`STATIC_ANALYSIS_STRICT=1`), exit non-zero
- In non-strict mode (default), skip that tool and continue

### 2. Strict mode flag

```makefile
STATIC_ANALYSIS_STRICT ?= 0
```

CI sets `STATIC_ANALYSIS_STRICT=1` in the workflow. Local builds default to 0.

### 3. Per-tool targets

Split into `make cppcheck` and `make clang-tidy` so contributors can run individual tools. The combined `make static-analysis` runs both.

### Changes

| File | Change |
|------|--------|
| `src/Makefile` | Add preflight checks, STATIC_ANALYSIS_STRICT flag, per-tool targets |
| `.github/workflows/ci.yml` | Set STATIC_ANALYSIS_STRICT=1 in static-analysis job |

## Acceptance Criteria

- [ ] `make static-analysis` with missing cppcheck: prints diagnostic, exits 0 locally
- [ ] `make static-analysis` with missing cppcheck in CI: prints diagnostic, exits 1
- [ ] `make cppcheck` and `make clang-tidy` work as standalone targets
- [ ] Existing CI behavior unchanged (still fails on analysis errors)

## Owner and Effort

- **Owner:** TBD
- **Effort:** S (Makefile changes only)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct Makefile + CI change.
- **Rollback:** git revert.
- **Blast radius:** Build system only. No code changes.

## Test Plan

- [ ] Manual: run `make static-analysis` without cppcheck installed
- [ ] Manual: run `make static-analysis` with cppcheck installed (existing behavior)
- [ ] CI: verify STATIC_ANALYSIS_STRICT=1 still catches real errors

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** None.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Static analysis robustness | P3 | S | Developer experience |

## Trade-offs

**Why not just require the tools?** Not all contributors have cppcheck/clang-tidy installed. Failing with a cryptic error is worse than skipping with a clear message.

**Why not auto-install the tools?** Makefiles should not install system packages. That is the user's or CI's responsibility.
