# Proposal: Fuzzing and Sanitizer CI Gates

## Problem

aimee is a C codebase parsing untrusted input (JSON over sockets, config files,
agent output). Memory safety bugs in parsing paths can cause crashes or
exploitable conditions. Current state:

1. **No sanitizer enforcement in CI.** ASan/UBSan may be used locally but are not
   gating PR merges.
2. **No fuzz targets.** JSON parsing (`cJSON_Parse`), message framing, config
   loading, and memory search queries are all fuzz-worthy attack surfaces.
3. **No coverage tracking.** There is no visibility into which code paths are
   exercised by tests vs which are untested.

## Goals

- ASan and UBSan run on every PR and block on regressions.
- Fuzz targets cover JSON parsing, message framing, and config loading.
- Fuzz corpus smoke tests run on every PR; extended fuzzing runs nightly.
- Coverage floor enforced for high-risk modules.

## Approach

### 1. Sanitizer CI jobs

Add CI jobs that compile and test with:
- `-fsanitize=address` (ASan): heap/stack buffer overflow, use-after-free, double-free
- `-fsanitize=undefined` (UBSan): signed overflow, null deref, alignment, shift

```makefile
sanitize: CC=clang CFLAGS+=-fsanitize=address,undefined
	$(MAKE) test
```

Run on every PR. Fail on any sanitizer finding.

**Platform note:** ASan/UBSan are supported by both GCC and Clang on Linux and
macOS. On Windows, MSVC has `/fsanitize=address` support. CI matrix should run
sanitizers on Linux (primary) and macOS (when Phase 1 portability lands).

### 2. Fuzz targets

Create fuzz harnesses using libFuzzer (Clang) or AFL:

| Target | Input | Module |
|--------|-------|--------|
| `fuzz_json_parse` | Raw JSON bytes | `server.c:server_dispatch()` |
| `fuzz_message_frame` | Newline-delimited message stream | `server.c` read path |
| `fuzz_config_load` | Config JSON file contents | `config.c:config_load()` |
| `fuzz_memory_search` | Search query strings | `memory.c:memory_search()` |
| `fuzz_acl_parse` | CIDR strings | `webchat.c` ACL parser |

Each target:
1. Initializes minimal required state (db, config)
2. Feeds fuzzed input to the target function
3. Verifies no crash, no sanitizer finding

### 3. Corpus management

- Seed corpus: extract representative inputs from tests and real usage
- Store in `tests/fuzz/corpus/<target>/`
- CI runs corpus smoke test (all corpus inputs, ~5 seconds) on every PR
- Nightly: 30-minute extended fuzz run per target
- New crash inputs auto-minimized and added to corpus

### 4. Coverage tracking

Add `make coverage` target using `gcov`/`llvm-cov`:
- Generate HTML coverage report
- Enforce coverage floor for high-risk files:
  - `server.c`: ≥80%
  - `server_auth.c`: ≥90%
  - `config.c`: ≥80%
  - `memory.c`: ≥70%

### Changes

| File | Change |
|------|--------|
| `src/Makefile` | Add `sanitize`, `fuzz`, `fuzz-smoke`, `coverage` targets |
| `src/tests/fuzz/` | New directory: fuzz harnesses and seed corpus |
| `.github/workflows/` | Add sanitizer and fuzz CI jobs |

## Acceptance Criteria

- [ ] CI runs ASan+UBSan on every PR and blocks on findings
- [ ] Fuzz targets exist for JSON parse, message frame, config load, memory search
- [ ] Corpus smoke test runs on every PR (<10s)
- [ ] Nightly extended fuzz runs for 30 minutes per target
- [ ] Crash inputs are auto-minimized and committed to corpus
- [ ] Coverage report generated; floors enforced for high-risk modules

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Sanitizer jobs added to CI immediately. Fuzz targets added incrementally.
- **Rollback:** Remove CI jobs. No runtime impact.
- **Blast radius:** Sanitizer findings may block PRs that were previously passing. This is intentional — those PRs have latent bugs.

## Test Plan

- [ ] Verify ASan detects a known buffer overflow in a test case
- [ ] Verify UBSan detects a known undefined behavior case
- [ ] Verify fuzz targets run without false positives on seed corpus
- [ ] Verify coverage report generates and floor check works

## Operational Impact

- **Metrics:** Coverage percentage tracked per module.
- **Logging:** None at runtime.
- **Alerts:** CI failure on sanitizer finding or coverage regression.
- **Disk/CPU/Memory:** Sanitizer builds use ~2x memory. Fuzz runs are CPU-intensive but confined to CI.

## Priority

P2 — proactive defense against memory safety bugs in C code.

## Trade-offs

**Why libFuzzer over AFL?** libFuzzer integrates directly with sanitizers and
runs in-process (faster feedback loop). AFL is an alternative that works with
any binary. Both are valid; libFuzzer is preferred for Clang-based builds.

**Why coverage floors instead of coverage targets?** Floors prevent regression
(coverage can't drop below N%). Targets encourage gaming (adding trivial tests
to hit a number). Floors are the more useful metric.
