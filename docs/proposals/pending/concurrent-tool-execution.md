# Proposal: Concurrent Tool Execution

## Problem

When an LLM response contains multiple tool calls (e.g., read 3 files, or grep + read_file simultaneously), aimee executes them sequentially in `agent.c`. This wastes time — independent tool calls like reading different files or running independent shell commands could execute in parallel.

Mistral-vibe implements `_run_tools_concurrently()` using async queues: all tool calls from a single response are dispatched in parallel, results are collected via a queue, and events are streamed back as they complete. This significantly reduces turn latency for multi-tool responses.

## Goals

- Independent tool calls within a single LLM response execute concurrently.
- Tool results are collected and returned in the original call order.
- Dependent tools (e.g., file write followed by verification) remain sequential when flagged.
- Concurrent execution works identically in CLI chat and webchat.
- Maximum parallelism is configurable (default: 4).

## Approach

### Thread Pool

Use a small thread pool (default 4 threads) to execute tool calls concurrently. Each tool call runs in its own thread with its own stack. Results are written to a shared result array indexed by call order.

```c
typedef struct {
    int index;                  /* original call order */
    parsed_tool_call_t call;    /* tool name + args */
    char result[MAX_TOOL_RESULT]; /* output */
    int status;                 /* 0=ok, -1=error */
} concurrent_tool_task_t;
```

### Safety Classification

Not all tools are safe to run concurrently:

| Category | Tools | Concurrent? |
|----------|-------|-------------|
| Read-only | read_file, grep, list_files, git_log | Yes |
| Write (independent files) | write_file (different paths) | Yes |
| Write (same file) | write_file (same path) | No — serialize |
| Shell | bash | Configurable (default: no) |
| State-modifying | git_commit, git_push | No — serialize |

The tool registry already has an `enabled` flag per tool. Add a `concurrent_safe` flag that defaults based on tool category. Write tools check for path conflicts before allowing parallel execution.

### Execution Flow

1. LLM returns N tool calls.
2. Classify each: concurrent-safe or must-serialize.
3. Dispatch concurrent-safe calls to thread pool.
4. Execute must-serialize calls sequentially after concurrent batch completes.
5. Merge results in original call order.
6. Return to agent loop.

### Changes

| File | Change |
|------|--------|
| `src/headers/agent_tools.h` | Add concurrent execution types and API |
| `src/agent_tools.c` | Add thread pool, concurrent dispatch, result collection |
| `src/agent.c` | Replace sequential tool loop with concurrent dispatcher |
| `src/agent_policy.c` | Add `concurrent_safe` flag to tool registry |
| `src/webchat.c` | Use same concurrent dispatcher for webchat tool execution |

## Acceptance Criteria

- [ ] 3 independent `read_file` calls execute in parallel (wall time < 2x single call)
- [ ] `write_file` to different paths executes in parallel
- [ ] `write_file` to the same path serializes correctly
- [ ] `bash` calls respect the concurrent config (default: serial)
- [ ] Results are returned in the original call order regardless of completion order
- [ ] Thread pool size configurable via `agent.concurrent_tools` (default: 4)
- [ ] CLI and webchat use the same concurrent dispatcher
- [ ] No race conditions: thread sanitizer clean under concurrent load

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2-3 days)
- **Dependencies:** None (pthreads already used in codebase)

## Rollout and Rollback

- **Rollout:** Default enabled. `agent.concurrent_tools = 1` disables (sequential fallback).
- **Rollback:** Set concurrent_tools to 1. No persistent state changes.
- **Blast radius:** All agent tool execution. Must be thoroughly tested for race conditions.

## Test Plan

- [ ] Unit tests: thread pool lifecycle, task dispatch, result collection
- [ ] Integration tests: 4 concurrent reads, verify all results correct
- [ ] Integration tests: write conflict detection — same-path writes serialize
- [ ] Stress test: 16 concurrent tool calls with thread sanitizer enabled
- [ ] Failure injection: tool call crashes in thread — verify other calls unaffected
- [ ] Manual verification: observe timing improvement in multi-read turns

## Operational Impact

- **Metrics:** `tool_calls_concurrent`, `tool_calls_serialized`, `concurrent_batch_latency_ms`
- **Logging:** DEBUG for dispatch/collect, INFO for serialization decisions
- **Alerts:** None
- **Disk/CPU/Memory:** Slightly higher CPU from thread pool. Memory: ~64KB stack per thread × pool size.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Concurrent Tool Execution | P2 | M | High — reduces turn latency for multi-tool responses |

## Trade-offs

**Alternative: Async I/O (epoll/io_uring) instead of threads.** More efficient but much more complex to retrofit. Threads are simpler and sufficient for the expected concurrency (4-8 calls).

**Alternative: Always serialize for safety.** Current behavior. Wastes time on independent reads/greps which are the most common multi-call pattern.

**Known limitation:** Shell commands (`bash`) are serial by default because they may have side effects that interact. Power users can enable concurrent bash via config, accepting the risk.
