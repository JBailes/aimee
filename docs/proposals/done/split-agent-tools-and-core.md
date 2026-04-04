# Proposal: Agent Tool Safety and Concurrency Hardening

## Problem

The original proposal over-focused on file-size targets. The real issues still
visible in the current codebase are functional:

- checkpoint state in `agent_tools.c` is still global
- rollback helpers still exist without a clear per-invocation lifecycle
- filesystem policy enforcement still lives in ad hoc helpers instead of one
  shared path gate

Those are worth fixing even if the files are not mechanically split to an
arbitrary line count.

## Goals

- Make checkpoint state per invocation instead of global
- Ensure all filesystem-facing tools use one shared canonical path-policy check
- Keep any structural split subordinate to those correctness goals

## Approach

### 1. Move checkpoints to invocation-local state

Replace the current global checkpoint array with an execution-local context that
is created for one agent run and passed through tool dispatch.

### 2. Unify filesystem policy

Replace the tool-local `validate_file_path()` path with a shared guardrail-level
helper used by:

- `read_file`
- `write_file`
- `list_files`
- `grep`
- file-based `verify`

### 3. Split only where it reduces risk

If the checkpoint refactor or path-policy cleanup naturally produces a cleaner
module boundary, split there. File length by itself is not the driver.

## Changes

| File | Change |
|------|--------|
| `src/agent_tools.c` | Remove global checkpoint state and adopt per-run context |
| `src/guardrails.c` | Expose a shared canonical filesystem policy check |
| `src/agent.c` | Thread checkpoint context through tool execution |

## Acceptance Criteria

- [ ] No global mutable checkpoint state remains in the delegate tool path
- [ ] All filesystem tools use the same shared path-policy check
- [ ] Parallel agent runs cannot interfere with each other's checkpoints
- [ ] Negative tests cover traversal and symlink-escape attempts

## Owner and Effort

- **Owner:** aimee core
- **Effort:** M
- **Priority:** P1
