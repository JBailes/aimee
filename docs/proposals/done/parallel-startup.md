# Proposal: Parallel Startup Pipeline

## Problem

Despite the optimizations in PR #147 (async fetch, schema fast-path, etc.), aimee's startup path is still fundamentally sequential. The two heaviest operations — `build_session_context()` (5-20ms of DB queries + file reads) and `worktree_ensure()` (50-250ms of git subprocess calls) — run back-to-back even though they have no data dependency on each other.

Current startup waterfall (post-PR #147):

```
config_load ──► db_open ──► state_save ──► fork prune ──► build_session_context ──► [return]
                                                                                        │
main() resumes: session_state_load ──► worktree_ensure ──► session_state_save ──► execlp
                                            │
                                       background fetch fork
                                       git rev-parse (20-50ms)
                                       git worktree add (50-200ms)
```

Total serial time: ~80-300ms for the git operations alone, plus 5-20ms for context assembly, all happening sequentially. With multiple workspaces, `worktree_ensure` is called once per workspace, multiplying the git subprocess cost.

## Goals

- Overlap `build_session_context()` with worktree creation so they run concurrently.
- Overlap worktree creation across multiple workspaces (parallel per-workspace).
- Provider CLI (`claude`, etc.) launches as soon as possible; worktree readiness is not a prerequisite for read-only operations.
- Write operations (Edit, Write, Bash writes) are gated on worktree readiness — they block until the worktree exists.
- Reduce perceived startup time to: `max(context_assembly, worktree_creation)` instead of `sum(context_assembly, worktree_creation)`.

## Approach

### Phase 1: Parallel context + worktree (pthread)

Restructure `cmd_session_start()` and `main()` so that worktree creation runs on a background thread while context assembly happens on the main thread.

**Design:**

```c
/* Thread argument for background worktree creation */
typedef struct {
   session_state_t *state;
   int ws_index;        /* which worktree entry to create */
   int result;          /* 0=success, -1=failure */
} worktree_thread_arg_t;

static void *worktree_thread_fn(void *arg) {
   worktree_thread_arg_t *wta = arg;
   wta->result = worktree_ensure(&wta->state->worktrees[wta->ws_index]);
   return NULL;
}
```

In `main()`, after `cmd_session_start()` returns:

1. Spawn one pthread per worktree entry that needs creation (those with `created == 0`).
2. Immediately proceed to call `build_session_context()` (moved from `cmd_session_start` into `main()`), `workspace_load()`, and provider detection — none of these require the worktree to exist.
3. Before `execlp(provider)`, join all worktree threads. The provider CLI needs the worktree path set as cwd, so we must wait here.

This is safe because:
- `worktree_ensure()` operates on independent worktree entries (no shared mutable state between entries).
- `build_session_context()` only reads the database (no worktree access).
- The session state is not written until after all threads join.

**Estimated savings:** On a single-workspace config, the worktree creation (50-250ms) overlaps with context assembly (5-20ms), saving up to 20ms. On multi-workspace configs (2-3 workspaces), all worktrees create in parallel instead of sequentially, saving 50-200ms per additional workspace.

### Phase 2: Deferred worktree gate for write operations

Instead of blocking on worktree creation before launching the provider CLI, launch the provider immediately and gate write operations on worktree readiness.

**Design:**

Add a `worktree_ready` atomic flag and condition variable to the session state:

```c
/* In guardrails.h */
#include <pthread.h>
#include <stdatomic.h>

typedef struct {
   /* ... existing fields ... */
   atomic_int worktree_ready;        /* 0=pending, 1=ready, -1=failed */
   pthread_mutex_t wt_mutex;
   pthread_cond_t wt_cond;
} session_state_t;
```

- The background worktree thread sets `worktree_ready = 1` and signals the condvar when done.
- `pre_tool_check()` (the guardrail gate that runs before every tool execution) checks `worktree_ready`:
  - If the tool is read-only (`Read`, `Glob`, `Grep`, `Bash` with read-only command) → allow immediately, routing reads to the original repo path if the worktree isn't ready yet.
  - If the tool is a write (`Edit`, `Write`, `Bash` with write command) → `pthread_cond_wait()` on `wt_cond` until `worktree_ready == 1`. This blocks the specific write operation, not the entire session.
- The provider CLI launches immediately and can begin processing the system prompt, reading files, etc. while the worktree finishes creating in the background.

**Estimated savings:** The entire worktree creation time (50-250ms) is hidden behind provider CLI startup, which itself takes 500ms+ for the LLM to begin responding. The user sees zero additional latency from worktree creation.

### Phase 3: Parallel context assembly sections

`build_session_context()` assembles 7 independent sections:
1. Rules generation (`rules_generate(db)`) — DB queries
2. Key facts (`SELECT ... FROM memories`) — DB query
3. Network summary (`agent_load_config()`) — file read
4. Workspace project context (`config_load` + DB queries) — file + DB
5. Delegation config (`agent_load_config()`) — file read (cached from #3)
6. Recent delegations (`SELECT ... FROM agent_log`) — DB query
7. Workspace build context (`workspace_load` + `workspace_build_context`) — file reads
8. Capabilities text (`build_capabilities_text()`) — pure computation

Sections 3, 7, and 8 have no database dependency. Sections 1, 2, 4, and 6 all query the database but on different tables with no write contention (read-only SELECTs).

SQLite in WAL mode supports concurrent readers, so sections 1, 2, 4, and 6 can run in parallel if each has its own `sqlite3` connection (or if using `SQLITE_OPEN_FULLMUTEX` serialized mode, which aimee already uses by default).

**Design:** Open a second read-only db connection for the thread pool. Partition the 8 sections into 2-3 groups and run them on separate threads:

- Thread A (main): Rules + key facts + project context + recent delegations (DB-heavy, same connection)
- Thread B: Network summary + delegation config + workspace context + capabilities text (file I/O, no DB needed)

Reassemble the sections in order after both threads complete.

**Estimated savings:** 2-5ms on fast systems, 10-20ms on systems with slow file I/O. Modest, but compounds with the other parallelism.

### Changes

| File | Change |
|------|--------|
| `src/main.c` | Move `build_session_context()` call out of `cmd_session_start()` into `main()`. Spawn worktree threads before context assembly, join after. |
| `src/cmd_hooks.c` | Extract `build_session_context()` as a public function. Remove context output from `cmd_session_start()` (it only sets up state now). |
| `src/headers/commands.h` | Declare `build_session_context()` as public |
| `src/headers/guardrails.h` | Add `atomic_int worktree_ready`, `pthread_mutex_t`, `pthread_cond_t` to `session_state_t` |
| `src/guardrails.c` | In `pre_tool_check()`: if write tool and `worktree_ready == 0`, wait on condvar. In `worktree_ensure()`: signal condvar on completion. |
| `src/cmd_hooks.c` | Split `build_session_context()` into parallel sections (Phase 3) |

## Acceptance Criteria

- [ ] `worktree_ensure()` runs concurrently with `build_session_context()` (verified via strace showing overlapping syscalls)
- [ ] Multi-workspace configs create all worktrees in parallel (verified: 3-workspace startup < 1.5x single-workspace)
- [ ] Read-only tool calls succeed before worktree is ready (reads route to original repo)
- [ ] Write tool calls block until worktree is ready (not rejected — blocked)
- [ ] No data races (verified via `make sanitizers` with ThreadSanitizer)
- [ ] Provider CLI launches before worktree creation completes
- [ ] `time aimee session-start` < 500ms on warm single-workspace system
- [ ] All existing tests pass
- [ ] No deadlocks: worktree thread always terminates (success or failure), condvar always signaled

## Owner and Effort

- **Owner:** JBailes
- **Effort:** M (Phase 1: S, Phase 2: M due to guardrail integration, Phase 3: S)
- **Dependencies:** PR #147 (startup-latency-v2) must land first.

## Rollout and Rollback

- **Rollout:** Direct code changes, no feature flags. Each phase can land independently.
- **Rollback:** `git revert` of any phase. Phase 2 reverts cleanly because the write gate is additive — removing it just means writes work immediately (pre-existing behavior).
- **Blast radius:** All sessions. Phase 2 changes the behavior of write operations (they may block briefly), but this is invisible to users since the block is shorter than the time it takes for the LLM to produce its first write.

## Test Plan

- [ ] Unit tests: worktree thread creates worktree concurrently with main thread work
- [ ] Unit tests: `pre_tool_check` blocks write tools when `worktree_ready == 0`, allows when `== 1`
- [ ] Unit tests: `pre_tool_check` allows read tools regardless of `worktree_ready` state
- [ ] Unit tests: condvar is signaled on both success and failure paths
- [ ] Integration tests: `aimee session-start` with 3 workspaces completes in parallel time, not serial
- [ ] Integration tests: provider CLI launches and can process reads before worktree finishes
- [ ] Stress test: 10 concurrent worktree creations don't deadlock or corrupt state
- [ ] ThreadSanitizer: `make sanitizers` passes with no data race reports
- [ ] Failure injection: `git worktree add` fails → condvar still signaled, write tools get error, session continues

## Operational Impact

- **Metrics:** None.
- **Logging:** Worktree thread logs creation time to stderr for debugging.
- **Alerts:** None.
- **Disk/CPU/Memory:** Slightly higher peak CPU (2-3 threads briefly active). One additional pthread stack (~8MB default, can reduce to 1MB via `pthread_attr_setstacksize`). No disk impact.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Phase 1: Parallel context + worktree | P0 | S | Saves 50-250ms per additional workspace; overlaps context with creation |
| Phase 2: Deferred worktree gate | P0 | M | Hides all worktree latency behind provider CLI startup |
| Phase 3: Parallel context sections | P2 | S | Saves 2-20ms; diminishing returns |

## Trade-offs

**pthreads vs fork:** Threads share memory, making the condvar-based write gate natural. Fork would require IPC (pipe/signal) to communicate readiness. Threads also avoid duplicating the sqlite connection and process state. Downside: pthreads require careful attention to thread safety — but the shared state is minimal (one atomic int, one condvar).

**Blocking writes vs rejecting writes:** We block (wait) rather than reject. Rejecting would cause the agent to retry or fail, wasting tokens and creating a confusing UX. Blocking is invisible because the LLM hasn't even generated its first write by the time the worktree is ready (worktree creation: 50-250ms; LLM first response: 500ms+).

**Phase 3 diminishing returns:** Parallelizing `build_session_context()` internally saves at most 20ms. It adds complexity (second DB connection, thread synchronization for buffer assembly). Worth doing only if Phases 1-2 are insufficient. Can be deferred indefinitely.

**SQLite thread safety:** SQLite in WAL mode supports unlimited concurrent readers. We open a second connection for the context assembly thread (not sharing the same `sqlite3*` handle across threads, which would require `SQLITE_OPEN_FULLMUTEX`). This is the recommended SQLite multi-threading pattern.
