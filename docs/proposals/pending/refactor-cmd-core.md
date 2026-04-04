# Proposal: Refactor Monolithic cmd_core.c

## Problem

`src/cmd_core.c` is currently the largest file in the codebase, spanning over 2,280 lines and containing approximately 43 distinct functions. This monolithic structure makes navigation difficult, increases cognitive load for developers, and raises the likelihood of painful merge conflicts when multiple features or bug fixes touch core commands simultaneously.

## Goals

- Improve code maintainability and readability by breaking down `cmd_core.c` into smaller, domain-specific modules.
- Reduce the average file size in the `src` directory to improve modularity.

## Approach

Categorize the core commands defined in `cmd_core.c` into logically related groupings (e.g., file operations, system execution, agent state management). Create new source files for each group and extract the corresponding functions. 

A central header file (`cmd_core.h`) will declare the entry points for these commands, which can then be registered with the central command table or router.

### Changes

| File | Change |
|------|--------|
| `src/cmd_core.c` | Remove extracted functions, potentially leaving only initialization/routing logic if needed. |
| `src/cmd_core_files.c` | (New File) Implement file reading, writing, and listing commands. |
| `src/cmd_core_system.c` | (New File) Implement shell execution and system-level commands. |
| `src/cmd_core_state.c` | (New File) Implement agent state and configuration commands. |

## Acceptance Criteria

- [ ] `src/cmd_core.c` is significantly reduced in size (target < 500 lines).
- [ ] New module files (`src/cmd_core_*.c`) are created and integrated into the build system (`Makefile`).
- [ ] All existing tests pass, verifying that the refactoring has not broken command functionality.

## Owner and Effort

- **Owner:** AI Agent
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct merge into `main` after thorough testing.
- **Rollback:** Revert commit.
- **Blast radius:** All core commands are affected; if a command fails to register or route correctly, core agent capabilities will be degraded.

## Test Plan

- [ ] Unit tests: Verify that all refactored commands still pass their existing unit tests.
- [ ] Integration tests: Run the full test suite to ensure no regressions in core workflows.
- [ ] Manual verification: Manually test a representative sample of core commands (e.g., file editing, shell execution).

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** Minor impact on binary size; no expected runtime performance changes.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Refactor `cmd_core.c` | P2 | M | Medium |

## Trade-offs

- Breaking the file apart requires updating the build system and ensuring header dependencies are managed correctly. It may temporarily disrupt in-flight PRs that heavily modify `cmd_core.c`.