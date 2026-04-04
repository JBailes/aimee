# Proposal: Idempotent Tool Result Caching

## Problem

Many tool calls within a session (e.g., `read_file`, `list_files`, `grep`, `git_log`) are repeated by the agent to verify state or because they've forgotten the previous turn's output. For large repos, re-running these tools consumes unnecessary tokens (if the results are re-inserted into the context) and increases latency.

## Goals

- Eliminate redundant execution of read-only tools.
- Reduce agent turn latency for repeated lookups.
- Avoid duplicate results in session history if the underlying file state has not changed.

## Approach

Implement a `tool_cache` layer in `agent_tools.c` that checks for a matching result in the database before executing a tool marked as `idempotent` in the `tool_registry`.

1.  **Cache Key Generation**: Use a combination of `tool_name` + `arguments_json`.
2.  **State Validation**:
    *   For `read_file`, include the target file's `st_mtime` and `st_size` in the cache key.
    *   For `list_files`, include the directory's `st_mtime`.
    *   For `git_log`, include the latest commit hash (HEAD) of the repository.
3.  **Storage**: Use a new `tool_result_cache` table in `db.c` with a TTL (e.g., 10 minutes) to prevent the database from growing indefinitely.

### Changes

| File | Change |
|------|--------|
| `src/agent_tools.c` | Add `tool_cache_get` and `tool_cache_put` calls in `dispatch_tool_call` |
| `src/db.c` | Add `tool_result_cache` table with `hash`, `result`, `mtime_check`, `expires_at` |
| `src/headers/aimee.h` | Define `TOOL_CACHE_TTL` |

## Acceptance Criteria

- [ ] Sequential `read_file` calls for the same file (without modifications) return the cached result in < 1ms.
- [ ] If a file is modified (via `write_file` or external editor), the cache is correctly bypassed.
- [ ] `git_log` results are cached until a new commit is detected.

## Owner and Effort

- **Owner:** Backend Developer
- **Effort:** S (2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** On by default; can be bypassed with `AIMEE_NO_CACHE=1`.
- **Rollback:** Revert `agent_tools.c` changes or delete the `tool_result_cache` table.
- **Blast radius:** Minimal; affects tool output speed. If invalidation fails, the agent may see stale code.

## Test Plan

- [ ] Unit tests: Verify cache hit after first call and miss after `utime()` modification of a file.
- [ ] Integration tests: Run an agent loop that calls `list_files` twice and verify only one actual filesystem scan occurs.
- [ ] Manual verification: Trace output should indicate `(cached)` for repeated tool calls.

## Operational Impact

- **Metrics:** `tool_cache_hits`, `tool_cache_misses`.
- **Disk/CPU/Memory:** Minimal increase in SQLite database size.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Tool Caching | P2 | S | Medium (Latency & Token Use) |

## Trade-offs

- **Staleness**: External changes (not made through `aimee`) may be missed for the duration of the TTL unless `mtime` checks are robust for all tools.
- **Storage**: Caching very large `grep` or `git_log` outputs may consume disk space.
