# Proposal: Enforce Structural Budgets and Ownership Guards

## Problem

aimee already has layered libraries in [src/Makefile], but the codebase continues to accumulate large files and weak boundaries. In the inspected tree:

- 157 `.c` and `.h` files account for about 68k lines
- several source files exceed 1,500 lines
- the largest non-vendored files include `webchat.c` (2622), `cmd_core.c` (2280), `cmd_agent.c` (1915), `agent_tools.c` (1809), `guardrails.c` (1687), and `mcp_server.c` (1581)

Without hard structural guardrails, cleanup is temporary. New features will continue landing in already-large files because the path of least resistance is editing the nearest monolith.

## Goals

- Prevent the largest files from continuing to grow unchecked.
- Enforce layer boundaries that already exist conceptually in the build.
- Make ownership and review expectations explicit for high-risk modules.
- Catch structural regressions in CI before they merge.

## Approach

Add lightweight structural policy checks to the repo and CI:

1. Define source file size budgets.
2. Maintain an allowlist for existing exceptions while they are being reduced.
3. Require explicit review/ownership for changes to structural hotspots.
4. Add dependency/layer checks so command/UI code cannot silently reach across the architecture.

This proposal is intended to complement, not replace, refactoring work. It creates the guardrails needed to keep refactors from eroding.

### Changes

| File | Change |
|------|--------|
| `.github/workflows/ci.yml` | Add a structural policy job |
| `src/tests/test_build_integrity.sh` | Enforce source-file budgets and exception list |
| `src/Makefile` | Wire structural checks into `make lint` or a dedicated target |
| `docs/STATUS.md` | Track hotspot files and reduction progress |
| `docs/COMMANDS.md` | Document any command-surface deprecations or ownership rules if needed |
| `docs/proposals/pending/improve-module-boundaries.md` | Reference the structural policy as an enforcement dependency |
| `OWNERS.md` | New: define module owners/reviewers for hotspot areas |

## Acceptance Criteria

- [ ] CI fails when a non-exempt source file exceeds the agreed size budget.
- [ ] Existing oversized files are tracked in an explicit exception list with owners.
- [ ] Structural policy checks run locally and in CI.
- [ ] Changes to hotspot modules require named ownership/review metadata.
- [ ] New files cannot introduce layer violations without a reviewed exception.

## Owner and Effort

- **Owner:** aimee core
- **Effort:** S-M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Start in warning/report-only mode for one PR cycle, then switch to blocking once thresholds and exceptions are tuned.
- **Rollback:** Revert the CI job and local structural-check target.
- **Blast radius:** CI and developer workflow only; no runtime behavior changes.

## Test Plan

- [ ] Unit tests: none required beyond shell test coverage for the structural-check script.
- [ ] Integration tests: CI job validates failure on a synthetic oversized file in a test fixture or scripted check.
- [ ] Failure injection: malformed exception file, missing owner entry, intentionally introduced layer violation.
- [ ] Manual verification: run the structural check locally and confirm hotspot reporting matches the current tree.

## Operational Impact

- **Metrics:** Optionally publish hotspot counts and max-file-size trend in CI artifacts.
- **Logging:** CI logs report threshold failures and owner information.
- **Alerts:** None.
- **Disk/CPU/Memory:** Negligible CI cost.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Structural budgets in CI | P1 | S | Prevents further codebase sprawl |
| Ownership guardrails for hotspots | P2 | S | Improves review quality and accountability |

## Trade-offs

Hard thresholds can feel arbitrary if introduced without an exception mechanism, so the proposal includes a transition path and explicit exemptions. An alternative is relying on reviewer judgment alone, but the current code shape shows that soft norms have not been enough to stop hotspot growth.
