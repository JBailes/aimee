# Proposal: Implement Dynamic String Handling to Reduce Fixed-Size Buffers

## Problem

The codebase heavily relies on fixed-size stack arrays for string manipulation (e.g., `char id[64];`, `char new_entry[512];`, `char tool_sig[256];`). A static analysis revealed over 550 instances of this pattern. While stack allocation is fast and avoids memory leaks, using magic-numbered fixed buffers leads to several issues:
- **Silent Truncation:** Strings exceeding the buffer size (e.g., long file paths or tool signatures) may be silently truncated if `snprintf` is used, leading to subtle bugs.
- **Buffer Overflows:** If `sprintf` or `strcat` are used without proper bounds checking, these buffers become security vulnerabilities.
- **Wasted Memory:** Over-allocating large buffers just in case (e.g., `char buffer[4096];`) wastes stack space.

## Goals

- Eliminate the reliance on arbitrary fixed-size buffers for strings with unpredictable lengths (e.g., user input, file paths, tool signatures).
- Introduce a safe, ergonomic dynamic string abstraction to prevent truncation and overflow issues.

## Approach

Introduce a lightweight dynamic string library (similar to Redis's SDS or a simple custom struct with `char *data`, `size_t len`, `size_t capacity`). Replace fixed-size arrays with this dynamic string type in areas where string length is variable or difficult to predict safely.

### Changes

| File | Change |
|------|--------|
| `src/util_string.c` | (New File) Implement the dynamic string library functions (creation, appending, formatting, freeing). |
| `src/agent.c` | Replace fixed-size buffers (e.g., `tool_sig`, `new_entry`) with dynamic strings. |
| `src/server_main.c` | Replace fixed-size buffers used for request/response handling. |
| Various | Iteratively replace fixed-size arrays where truncation is a risk. |

## Acceptance Criteria

- [ ] A dynamic string library is implemented and unit tested.
- [ ] High-risk fixed-size buffers (e.g., those handling file paths or user input) are replaced with dynamic strings.
- [ ] No memory leaks are introduced by the new dynamic strings (verified via Valgrind or similar tools).

## Owner and Effort

- **Owner:** AI Agent
- **Effort:** L
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct merge into `main`. This can be done iteratively (one module at a time).
- **Rollback:** Revert commits.
- **Blast radius:** Widespread string manipulation; incorrect usage of the new library could introduce memory leaks or use-after-free bugs.

## Test Plan

- [ ] Unit tests: Exhaustively test the dynamic string library (allocation, appending, resizing, freeing).
- [ ] Integration tests: Verify that areas updated to use dynamic strings (e.g., tool execution, file reading) still function correctly, especially with very long inputs.
- [ ] Memory Profiling: Run the test suite under Valgrind or AddressSanitizer to ensure no leaks or memory errors are introduced.

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** Slight increase in heap allocations and memory fragmentation; slight decrease in stack usage.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Implement Dynamic Strings | P2 | L | High |

## Trade-offs

- Moving from stack allocation to heap allocation for strings introduces minor performance overhead due to `malloc`/`realloc`/`free` calls. It also requires careful memory management to avoid memory leaks, making the code slightly more complex to write (requiring explicit cleanup). However, the trade-off is worth the increased safety and robustness against arbitrary input lengths.