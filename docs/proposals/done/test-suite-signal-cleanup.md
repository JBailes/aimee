# Proposal: Remove and Rewrite Low-Signal Tests

## Problem

Some existing tests pass without meaningfully protecting production behavior. They consume review attention and CI time while giving weak or misleading signals.

The main low-signal cases today are:

- `src/tests/test_bench_harness.c`, which copies benchmark helper logic inline instead of exercising production code
- `src/tests/test_workspace.c`, which contains at least one branch that explicitly does not assert anything
- parts of `src/tests/test_agent.c`, which validate very weak conditions such as "listing `/tmp` returns something"
- parts of `src/tests/test_compute_pool.c`, which treat "did not hang" as proof of parallel behavior
- shell smoke tests that check broad substring matches instead of asserting JSON structure or exact behavior

These tests are not equivalent in quality. Some should be deleted outright, some should be rewritten, and some should be reclassified as smoke tests rather than treated as meaningful coverage.

## Goals

- Remove tests that only verify copied test code or tautologies.
- Rewrite weak tests so they assert real production behavior.
- Distinguish smoke tests from behavior-focused tests in naming and expectations.
- Improve suite signal quality without increasing total test count unnecessarily.

## Approach

Treat low-signal tests by category rather than applying one blanket rule.

### 1. Delete tests that only verify duplicated logic

Remove `src/tests/test_bench_harness.c`.

That file copies percentile and regression-threshold logic into the test file itself. If `bench_perf.c` changes incorrectly, this test can still pass because it never exercises the production implementation. That is negative maintenance value.

Replace it with one of:

- a production helper extracted from `bench_perf.c` into a shared module with a real unit test, or
- no unit test at all if the benchmark harness remains a standalone tool

Until one of those exists, the copied test should not stay in the suite.

### 2. Rewrite tests that intentionally avoid assertions

Tighten `src/tests/test_workspace.c` so every section has a deterministic precondition and an assertion. Specifically:

- the "missing manifest" case should chdir into a temp directory without a manifest and assert failure
- cleanup should avoid shelling out to `rm -rf` via `system()`
- each helper should validate error returns and persisted state, not just in-memory structs

The suite should not include sections that knowingly accept "may or may not fail depending on cwd."

### 3. Strengthen weak assertions in `test_agent.c`

Rewrite the weakest checks in `src/tests/test_agent.c`:

- `tool_list_files()` should assert returned entries match a controlled temp directory, not `/tmp`
- tool error-path tests should validate structured error fields, not just substring presence
- dispatch tests should assert exact tool selection and expected JSON shape

Keep the useful parts of the file. The problem is not that `test_agent.c` exists; the problem is that some assertions are too weak.

### 4. Make compute-pool tests deterministic

Strengthen `src/tests/test_compute_pool.c` by replacing timing heuristics with synchronization primitives where possible:

- use barriers or counters to prove multiple tasks started before shutdown
- assert queue overflow behavior precisely
- keep one smoke-style timing test only if needed, but do not treat it as the primary correctness check

Tests based only on sleeps are acceptable as a small supplement, not as the main proof.

### 5. Reclassify shell scripts as smoke tests

Keep `src/tests/test_cli.sh` and `src/tests/test_integration.sh`, but explicitly treat them as smoke tests:

- rename or document them as smoke coverage, not comprehensive behavioral coverage
- make build prerequisites explicit so they do not fail due to missing binaries
- tighten the highest-value assertions to inspect JSON fields rather than just grepping for words

These scripts are still useful for end-to-end confidence. They are just not a substitute for focused tests.

### 6. Add a review rule for new tests

New tests should fail review if they do any of the following:

- duplicate production logic instead of calling it
- assert only "non-empty output" where exact behavior is knowable
- depend on ambient cwd, global machine state, or uncontrolled directories like `/tmp`
- use sleeps as the only correctness proof where synchronization is available

This should become an explicit repository standard so the suite does not regress back toward low-signal coverage.

### Changes

| File | Change |
|------|--------|
| `src/tests/test_bench_harness.c` | Remove or replace with tests over extracted production helpers |
| `src/tests/test_workspace.c` | Rewrite weak sections to assert deterministic behavior |
| `src/tests/test_agent.c` | Strengthen weak file-listing and tool-error assertions |
| `src/tests/test_compute_pool.c` | Replace timing-only assertions with deterministic synchronization checks |
| `src/tests/test_cli.sh` | Reclassify as smoke test and tighten critical assertions |
| `src/tests/test_integration.sh` | Reclassify as smoke test and tighten critical assertions |
| `src/tests/Rules.mk` | Remove obsolete target or rename smoke targets if needed |
| `docs/COMMANDS.md` | Optionally document smoke-vs-unit test split if surfaced to contributors |

## Acceptance Criteria

- [ ] `test_bench_harness.c` is removed or replaced with a test that exercises production code
- [ ] `test_workspace.c` has no sections that intentionally avoid asserting outcomes
- [ ] `test_agent.c` no longer relies on uncontrolled filesystem listings for success
- [ ] `test_compute_pool.c` proves concurrency behavior with deterministic assertions
- [ ] Shell scripts are clearly designated as smoke tests and have explicit build prerequisites
- [ ] New tests added under this policy avoid copied logic, tautological assertions, and ambient-environment dependencies

## Owner and Effort

- **Owner:** aimee core
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Land in one cleanup PR or a short sequence of test-only PRs. Remove or rewrite low-signal tests before adding many new ones.
- **Rollback:** Revert the cleanup changes. No production behavior or state is affected.
- **Blast radius:** CI and contributor workflow only. The main risk is temporarily losing smoke coverage if removal and replacement are not sequenced carefully.

## Test Plan

- [ ] Unit tests: verify rewritten workspace, agent, and compute-pool tests pass reliably across repeated runs
- [ ] Integration tests: smoke scripts still cover repo-root binary startup and basic server/client flow
- [ ] Failure injection: intentionally break a targeted behavior and verify the rewritten test fails for the right reason
- [ ] Manual verification: run `make unit-tests`, `./src/tests/test_cli.sh`, and `./src/tests/test_integration.sh` from documented entry points

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** Slight CI improvement if copied or redundant tests are removed; negligible otherwise.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Remove copied benchmark harness test | P1 | S | Eliminates misleading coverage |
| Rewrite workspace test | P1 | S | Improves determinism and trust |
| Strengthen agent test assertions | P2 | S | Better tool-layer regression detection |
| Strengthen compute-pool tests | P2 | S | Better concurrency confidence |
| Reclassify shell scripts as smoke | P2 | S | Better test taxonomy and expectations |

## Trade-offs

Removing weak tests can make the total test count go down before stronger replacements land. That is acceptable. A smaller suite with accurate failure modes is more valuable than a larger suite that hides risk behind green builds.

Some shell-based smoke coverage should remain because it catches packaging and binary wiring issues that unit tests do not. The trade is to keep those scripts narrow and honest about what they cover.
