# Proposal: C Codebase Safety and Quality Improvements

## Problem

- `src/platform_process.c` ignores the timeout parameter during child process execution (has a `TODO: implement timeout with alarm/waitpid loop`). This can lead to indefinite blocking if external commands hang or wait for input.
- `src/cmd_branch.c`, `src/mcp_git.c`, and `src/memory_context.c` use the unsafe `strcat` function instead of bounds-checked alternatives like `strncat` or `snprintf`, creating potential vectors for buffer overflow vulnerabilities.

## Goals

- Prevent the agent from blocking indefinitely when running external processes by enforcing `timeout_ms`.
- Eliminate all `strcat` calls from the codebase to proactively improve memory safety.

## Approach

- In `src/platform_process.c`, replace the naive blocking `waitpid` call with a polling loop utilizing `waitpid(..., WNOHANG)` and a small `nanosleep` to accurately enforce `timeout_ms`. If the timeout is reached, send `SIGTERM` (and subsequently `SIGKILL` if needed) to the child process.
- In `src/cmd_branch.c` (lines 278, 279), `src/mcp_git.c` (line 487), and `src/memory_context.c` (lines 331, 362), replace `strcat` with safe alternatives like `snprintf` or `strncat`, ensuring the destination buffer size is explicitly respected.

### Changes

| File | Change |
|------|--------|
| `src/platform_process.c` | Implement timeout polling loop in process execution. |
| `src/cmd_branch.c` | Replace `strcat` with `snprintf`/`strncat`. |
| `src/mcp_git.c` | Replace `strcat` with `snprintf`/`strncat`. |
| `src/memory_context.c` | Replace `strcat` with `snprintf`/`strncat`. |

## Acceptance Criteria

- [ ] `aimee` terminates external commands and returns a timeout error when `timeout_ms` is exceeded.
- [ ] No usages of `strcat` remain in the C codebase (`grep -rn "strcat(" src` returns 0).
- [ ] Existing tests pass without regression.

## Owner and Effort

- **Owner:** AI Agent
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct merge into `main`.
- **Rollback:** Revert commit.
- **Blast radius:** Only external process execution and string formatting in specific commands/contexts.

## Test Plan

- [ ] Unit tests: Add test for process timeout behavior using a mock sleeping script.
- [ ] Integration tests: Verify branch command output and context generation formats strings correctly without overflow.
- [ ] Manual verification: Run a long-sleeping shell command and observe `aimee` timing out gracefully.

## Operational Impact

- **Metrics:** None.
- **Logging:** Log a clear warning when a child process is terminated due to timeout.
- **Alerts:** None.
- **Disk/CPU/Memory:** Negligible CPU overhead added during the `waitpid` polling loop.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Process Timeout | P1 | S | High |
| `strcat` Removal | P2 | S | Medium |

## Trade-offs

- Polling `waitpid` with `WNOHANG` and `nanosleep` introduces minor latency in detecting process exit and consumes slightly more CPU than blocking `waitpid`, but provides the necessary millisecond-resolution timeout control without the complexity of asynchronous signals (like `alarm()`).