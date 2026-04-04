# Proposal: Quality Hardening (Remaining)

## Problem

The build/test pipeline is functional (CI, unit tests, integration tests,
sanitizers), but three verification categories are absent: fuzz testing, static
analysis, and coverage reporting. These gaps mean parser security properties are
unverified and code coverage is unknown.

Performance benchmarks and the security threat model were completed in PR #94
(see `docs/proposals/complete/quality-hardening-done.md`).

## Goals

- Parsers that handle untrusted input are fuzz-tested with seed corpora.
- Static analysis catches bugs at PR time with zero false-positive noise.
- Code coverage is measured and visible, with identified blind spots.

## Approach

### 1. Fuzz Testing

Fuzz the extractor parsers (JS, Python, Go, C, Lua, TypeScript, C#, Shell, CSS,
Dart), the JSON-RPC protocol handler (`server_dispatch` in `server.c:201`), and
the MCP stdio protocol handler (`handle_request` in `mcp_server.c:399`) with
random and mutated inputs.

Use libFuzzer or AFL. Create fuzz harnesses:

- `fuzz_extractors.c` — feeds random content to `extract_imports()`,
  `extract_exports()`, `extract_definitions()` across all extensions

Extractors are the priority target: they handle untrusted input (user source
files) and have the most complex parsing logic. Server dispatch and cJSON
handle locally-originated input (Unix socket from same-UID process) where the
threat model is weaker — these can be added as follow-up harnesses.

Seed corpora: Include real source files from each supported language (one small
file per language) under `src/tests/fuzz_corpus/`. This gives the fuzzer
meaningful starting inputs rather than pure random data.

Crash triage: Fuzzer crashes in CI create GitHub issues automatically. The issue
must be triaged within one business day (assign, reproduce, severity-classify).

Add Makefile target:

```makefile
fuzz: fuzz_extractors
    ./fuzz_extractors -max_total_time=60
```

CI integration: Add a `fuzz` job that runs the harness for 60 seconds. Not
blocking (informational), but failures create issues.

### 2. Static Analysis

Run `clang-tidy` and `cppcheck` on all non-vendored source files.

Add CI job:

```yaml
static-analysis:
  runs-on: ubuntu-latest
  steps:
    - uses: actions/checkout@v4
    - name: Install tools
      run: sudo apt-get install -y clang-tidy cppcheck
    - name: cppcheck
      run: |
        cppcheck --enable=warning,performance,portability \
          --suppress=missingIncludeSystem \
          --error-exitcode=1 -Isrc/headers -Isrc/vendor/headers \
          src/*.c
    - name: clang-tidy
      run: |
        clang-tidy src/*.c -- -Isrc/headers -Isrc/vendor/headers \
          -DWITH_PAM 2>&1 | tee tidy.log
        grep -c "warning:" tidy.log || true
```

Start non-blocking (warnings logged). Graduation criteria: promote to blocking
when the baseline has zero warnings for 2 consecutive weeks on main.

Note: `--enable=all` was considered but rejected — it enables `style` checks that
produce hundreds of low-value warnings and obscure real findings. Start with
`warning,performance,portability` and expand categories once the baseline is clean.

### 3. Coverage Reporting

Generate code coverage reports showing which lines/branches are exercised by unit
+ integration tests. Current test count: 18 unit modules, 33 CLI tests.

Build with `--coverage` flags:

```makefile
coverage:
    make clean
    make unit-tests EXTRA_C_FLAGS="--coverage" EXTRA_L_FLAGS="--coverage"
    lcov --capture --directory build/obj --output-file coverage.info
    lcov --remove coverage.info '*/vendor/*' '*/tests/*' --output-file coverage.info
    genhtml coverage.info --output-directory coverage-report
```

CI integration: Add coverage job, upload `coverage-report/` as artifact.
Optionally post coverage % as PR comment.

Stretch — mutation testing: Use `mull` or `universalmutator` (C-compatible
mutation testing tools) to verify tests catch regressions, not just exercise
code paths.

### Changes

| Deliverable | Files |
|-------------|-------|
| Fuzz harness + corpus | `src/tests/fuzz_extractors.c`, `src/tests/fuzz_corpus/` |
| Static analysis CI | `.github/workflows/ci.yml` (new `static-analysis` job) |
| Coverage reporting | Makefile `coverage` target, CI job |

## Acceptance Criteria

- [ ] `make fuzz` runs extractors fuzzer for 60s without crash
- [ ] Fuzz corpus contains at least one seed file per supported language
- [ ] `cppcheck` CI job runs on every PR with `--enable=warning,performance,portability`
- [ ] `clang-tidy` CI job runs on every PR (non-blocking initially)
- [ ] Static analysis promoted to blocking after 2 weeks with zero warnings on main
- [ ] `make coverage` generates an HTML report; coverage % is visible in CI artifacts

## Owner and Effort

- **Owner:** TBD
- **Effort:** M (static analysis and coverage are low effort; fuzzing is medium)
- **Dependencies:** None — all items are independent of each other and of other proposals

## Rollout and Rollback

- **Rollout:** CI jobs added to `.github/workflows/ci.yml`. Fuzzing and static
  analysis start non-blocking. Coverage is informational-only.
- **Rollback:** Remove CI job definitions. No runtime behavior changes.
- **Blast radius:** CI only. No impact on production binaries or user sessions.

## Test Plan

- [ ] Fuzz harnesses compile and run without crashing on seed corpus
- [ ] Static analysis job runs successfully on current codebase
- [ ] Coverage report generates and excludes vendor/test files
- [ ] Integration test: coverage report is generated and identifies at least one uncovered function

## Operational Impact

- **Metrics:** CI job pass/fail rates, coverage %, fuzz crash count.
- **Logging:** CI job output only.
- **Alerts:** Fuzz crashes create GitHub issues.
- **Disk/CPU/Memory:** CI runner time increases by ~3 minutes per PR.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Static analysis CI | P0 | S | Catches bugs at PR time, lowest effort highest signal |
| Fuzz testing (extractors) | P1 | S | Finds crashes in untrusted-input parsers |
| Coverage reporting | P2 | S | Discovery tool for identifying blind spots |

## Trade-offs

**Why not run fuzzers for longer in CI?** 60 seconds per harness is short but
catches the low-hanging crashes. Longer runs (hours) should be done periodically
as a scheduled job, not on every PR.

**Why not block PRs on coverage thresholds?** Coverage thresholds create
perverse incentives (writing tests to hit lines, not to verify behavior). Use
coverage as a discovery tool, not a gate.
