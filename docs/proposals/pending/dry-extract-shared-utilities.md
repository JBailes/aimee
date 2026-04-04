# Proposal: Extract Shared Utility Functions (DRY Cleanup)

## Problem

Several utility functions are duplicated across multiple source files in the aimee codebase:

1. **`run_cmd()`** — identical `popen` + `fread` + `pclose` wrapper appears in `mcp_git.c:33`, `git_verify.c:35`, and `cmd_branch.c:18`. All three do: `popen`, `fread` in a loop, null-terminate, `pclose`, return buffer.

2. **`shell_escape()`** — single-quote escaping for shell safety. Full function at `mcp_git.c:58`, inline copy-paste at `mcp_server.c:786-808`. The `mcp_server.c` version is missing the malloc-failure fallback.

3. **`is_sensitive_file()` / `sensitive_patterns`** — file sensitivity checking duplicated between `guardrails.c:24-31` (as `sensitive_patterns[]` array + matching functions) and `mcp_git.c:88-96` (as `is_sensitive_file()` with its own `patterns[]` array). Same patterns, same logic, two maintenance points.

4. **Extra header parsing** — `strtok_r` loop to split newline-separated HTTP headers duplicated at `agent_http.c:214-226` and `agent_http.c:335-347`.

5. **Regex compile/match/free** — the `regcomp` + `regexec` + `regfree` three-step pattern appears ~10 times in `memory.c` (`gate_check_sensitive`, `gate_check_ephemeral`, `gate_has_evidence_markers`, `memory_scan_content`). Each time it's 5-8 lines of boilerplate.

## Goals

- Each utility function exists in exactly one place
- No behavioral changes — pure refactor
- New shared functions are tested

## Approach

- Extract `run_cmd()` to `util.c` / `util.h` as the shared implementation
- Extract `shell_escape()` to `util.c` / `util.h`
- Extract `is_sensitive_file()` + `sensitive_patterns` to `guardrails.c` as the authoritative module and expose via `guardrails.h`
- Extract `parse_extra_headers()` as a static helper within `agent_http.c` since it is only used there
- Add `regex_match(pattern, text, flags) -> bool` helper to `util.c` / `util.h`
- Update all call sites to use the shared functions
- Delete the duplicate definitions

### Changes

| File | Change |
|------|--------|
| `src/util.c` | Add `run_cmd()`, `shell_escape()`, `regex_match()` |
| `src/headers/util.h` | Declare `run_cmd()`, `shell_escape()`, `regex_match()` |
| `src/mcp_git.c` | Remove static `run_cmd()` and `shell_escape()`, use `util.h` versions |
| `src/git_verify.c` | Remove static `run_cmd()`, use `util.h` version |
| `src/cmd_branch.c` | Remove static `run_cmd()`, use `util.h` version |
| `src/mcp_server.c` | Remove inline shell escape code at lines 786-808, use `shell_escape()` from `util.h` |
| `src/headers/guardrails.h` | Declare `is_sensitive_file()` |
| `src/guardrails.c` | Expose existing `is_sensitive_file()` (remove `static`) |
| `src/mcp_git.c` | Remove local `is_sensitive_file()`, include `guardrails.h` |
| `src/agent_http.c` | Extract `parse_extra_headers()` as local static to deduplicate the two copies |
| `src/memory.c` | Replace ~10 `regcomp`/`regexec`/`regfree` blocks with `regex_match()` calls |
| `src/tests/test_util.c` | Add unit tests for `run_cmd()`, `shell_escape()`, `regex_match()` |

## Acceptance Criteria

- [ ] Zero duplicated function bodies across the codebase for these 5 patterns
- [ ] All existing tests pass
- [ ] New unit tests for `run_cmd()`, `shell_escape()`, `regex_match()` in `test_util.c`
- [ ] `grep` confirms no remaining `static.*run_cmd` or `static.*shell_escape` definitions outside `util.c`

## Owner and Effort

- **Owner:** aimee core
- **Effort:** M
- **Dependencies:** none

## Rollout and Rollback

- **Rollout:** Compile and test
- **Rollback:** Revert commit
- **Blast radius:** All modules using these functions. Low risk since behavior is identical.

## Test Plan

- [ ] Unit tests: `run_cmd()` capturing stdout, `shell_escape()` with embedded quotes, `regex_match()` with representative patterns
- [ ] Integration tests: full test suite passes with no regressions
- [ ] Manual verification: `grep` search confirms no remaining duplicate definitions

## Operational Impact

- **Metrics:** None
- **Logging:** None
- **Disk/CPU/Memory:** No change

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Extract `run_cmd`, `shell_escape`, `regex_match` | P2 | M | Reduces maintenance burden, eliminates divergence risk |

## Trade-offs

Could go further and extract all `popen` usage to `safe_exec_capture` as part of a broader security hardening effort. This proposal keeps the change limited to a minimal DRY refactor. Security improvements are handled in the shell injection proposal.
