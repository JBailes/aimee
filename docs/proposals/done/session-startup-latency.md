# Proposal: Session Startup Latency Reduction

## Problem

Interactive session startup (`aimee` with no subcommand, or `aimee session-start`) takes 1-8 seconds due to four independent bottlenecks:

1. **Blocking worktree creation (1-5s).** `cmd_session_start()` in `src/cmd_hooks.c:777-820` forks a child per workspace to run `git worktree add`, then blocks on `waitpid()` for all of them before returning. Every session pays this cost even though most agent interactions only touch one worktree.

2. **100ms poll loop for socket readiness (0-2.7s).** `spawn_server()` in `src/cli_client.c:319-327` polls `stat()` every 100ms in a 30-iteration loop waiting for the server socket file to appear. On a fast machine the server is ready in <100ms but the client still sleeps until the next poll tick.

3. **libcurl init before socket creation (50-500ms).** `server_main.c:65` calls `agent_http_init()` (which runs `curl_global_init(CURL_GLOBAL_DEFAULT)`) before `server_init()` creates and binds the Unix socket. The client cannot connect until the socket exists, so curl init time directly adds to perceived startup latency.

4. **Unconditional FTS population (10-100ms).** `create_fts_tables()` in `src/db.c:695-700` runs `INSERT OR IGNORE INTO memories_fts … WHERE id NOT IN (SELECT rowid FROM memories_fts)` on every `db_open()`. When the FTS index is already in sync this does a full table scan for zero rows inserted.

## Goals

- Reduce cold-start latency to under 1 second for the common single-workspace case.
- Reduce cold-start latency to under 2 seconds for a 3-workspace config.
- Eliminate unnecessary blocking — return control to the agent as early as possible.
- Maintain correctness: worktrees must exist before any file operation uses them; the server must be connectable before the first RPC.

## Approach

### Defer worktree creation (M)

Instead of creating all worktrees synchronously during `cmd_session_start()`, return the session response immediately with worktree paths populated but creation deferred. Worktree creation happens lazily on first access.

**Design:**
- `cmd_session_start()` computes worktree paths and writes them to session state, but does NOT fork/exec `git worktree add`.
- Add a `worktree_ensure(session_state_t *state, const char *name)` function that creates the worktree on first call and is a no-op on subsequent calls. Use a `created` flag per `worktree_entry_t`.

**Central gate — mandatory worktree resolution:**

Rather than hunting for every call site, introduce `worktree_resolve_path()` as the single function that translates a workspace name to a usable path. Every existing path-resolution call site already goes through `session_state_t.worktrees[i].path` — replace direct `.path` access with `worktree_resolve_path(state, name)`, which calls `worktree_ensure()` internally. This makes it impossible to use a worktree path without ensuring it exists.

```c
/* Returns the worktree path, creating it on first access. Returns NULL on failure. */
const char *worktree_resolve_path(session_state_t *state, const char *name);
```

**Failure contract:**
- If `git worktree add` fails (e.g., branch conflict, disk full), `worktree_resolve_path()` returns NULL.
- Callers MUST check for NULL and propagate the error. `main()` prints a fatal error and exits. Delegate launch paths report failure to the parent agent.
- The `created` flag has three states: `0` (not attempted), `1` (created successfully), `-1` (creation failed — do not retry). Failed state persists in session state to avoid repeated fork/exec on a known-broken workspace.
- Concurrent calls are safe: `worktree_ensure()` uses a per-entry mutex (or atomic compare-and-swap on the `created` flag) to prevent double-creation races when multiple delegates resolve the same worktree simultaneously.

| File | Change |
|------|--------|
| `src/cmd_hooks.c` | Remove fork/waitpid worktree loop from `cmd_session_start()`; add `worktree_ensure()` and `worktree_resolve_path()` |
| `src/guardrails.c` | Add `created` flag (tri-state: 0/1/-1) to `worktree_entry_t`; serialize/deserialize it |
| `src/main.c` | Use `worktree_resolve_path()` before `chdir()` into worktree; fatal on NULL |

### inotify socket wait (S)

Replace the 100ms poll loop with `inotify` to wake immediately when the socket file is created.

**Design:**
- Before forking the server child, set up an `inotify` watch on the socket directory for `IN_CREATE`.
- After fork, `read()` on the inotify fd with a 3-second timeout (via `poll()` or `select()`).
- When the event fires, immediately `stat()` + `try_server()` as today.
- Fallback: if inotify setup fails, keep the existing poll loop.

| File | Change |
|------|--------|
| `src/cli_client.c` | Rewrite `spawn_server()` wait loop to use `inotify_init1()` + `inotify_add_watch()` + `poll()` |

### Move curl init after socket creation (S)

Reorder server startup so the socket is created and listening before libcurl is initialized. This lets clients connect (and queue in the listen backlog) while curl initializes.

| File | Change |
|------|--------|
| `src/server_main.c` | Move `agent_http_init()` from line 65 to after `server_init()` returns |

### Skip redundant FTS population (S)

Add an early-exit check so the FTS population query only runs when there are rows to insert.

**Design:**
- Before the `INSERT OR IGNORE`, run `SELECT COUNT(*) FROM memories WHERE id NOT IN (SELECT rowid FROM memories_fts)`.
- If count is 0, skip the insert entirely.

| File | Change |
|------|--------|
| `src/db.c` | Add `SELECT COUNT(*)` guard before the INSERT in `create_fts_tables()` |

## Acceptance Criteria

- [ ] `time aimee session-start` completes in <1s for a single-workspace config on a warm system
- [ ] `time aimee session-start` completes in <2s for a 3-workspace config on a cold system
- [ ] Worktree exists and is valid before any file operation attempts to use it (no race)
- [ ] Server socket accepts connections within 50ms of `spawn_server()` fork on a warm system
- [ ] FTS population query does not run when FTS index is already in sync (verified via sqlite3 trace)
- [ ] All existing integration tests pass without modification
- [ ] No regression in `make test` suite

## Owner and Effort

- **Owner:** JBailes
- **Effort:** M (deferred worktree is M due to multiple call sites; other three are S)
- **Dependencies:** None — all four changes are independent and can land in any order.

## Rollout and Rollback

- **Rollout:** Direct code changes, no feature flags. Each change is a separate commit.
- **Rollback:** `git revert` of any individual commit. No migrations or state changes.
- **Blast radius:** Deferred worktree and inotify affect session startup for all users. Curl reorder and FTS skip affect server startup and database open respectively.

## Test Plan

- [ ] Unit tests: `worktree_ensure()` creates worktree on first call, no-ops on second
- [ ] Unit tests: `worktree_ensure()` sets `created = -1` on failure and does not retry
- [ ] Unit tests: `worktree_resolve_path()` returns NULL when `created == -1`
- [ ] Unit tests: concurrent `worktree_resolve_path()` calls for the same workspace do not double-create (race test with threads)
- [ ] Unit tests: inotify wait returns immediately when socket exists, times out after 3s when it doesn't
- [ ] Integration tests: `aimee session-start` produces valid worktree paths that are usable
- [ ] Integration tests: `aimee delegate` can operate in a lazily-created worktree
- [ ] Integration tests: no code path accesses `worktree_entry_t.path` directly — all go through `worktree_resolve_path()` (grep audit)
- [ ] Failure injection: inotify unavailable (e.g., container without inotify) falls back to poll loop
- [ ] Failure injection: `git worktree add` fails — `worktree_resolve_path()` returns NULL, `main()` exits with clear error
- [ ] Failure injection: branch name conflict (worktree branch already exists) — handled gracefully
- [ ] Performance test: `time aimee session-start` completes under 1s (single workspace) and 2s (3 workspaces) in CI benchmark

## Operational Impact

- **Metrics:** No new metrics.
- **Logging:** `worktree_ensure()` will log to stderr on first creation for debuggability.
- **Alerts:** None.
- **Disk/CPU/Memory:** No change — same work is done, just deferred or done more efficiently.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Defer worktree creation | P1 | M | Saves 1-5s on every startup |
| inotify socket wait | P1 | S | Saves 0-2.7s on server spawn |
| Move curl init after socket | P2 | S | Saves 50-500ms on server spawn |
| Skip redundant FTS population | P2 | S | Saves 10-100ms on db open |

## Trade-offs

**Deferred worktree creation:** Shifts latency from startup to first use. If the agent immediately accesses the worktree (common case), the total time is the same but the startup *appears* faster because the session response returns immediately. The real win is multi-workspace configs where only one worktree is used per session.

**inotify vs poll:** inotify is Linux-only. macOS would need `kqueue` or `FSEvents`. Current approach: inotify with poll fallback. Future: abstract behind a platform layer if macOS support is needed.

**Curl init reordering:** The server will accept connections before curl is ready. If a request requiring HTTP arrives during this window, it will block until curl init completes. This is acceptable because curl init takes <500ms and the first real request won't arrive that fast.

**FTS early-exit:** The `SELECT COUNT(*)` adds a small overhead on the (rare) case where rows do need inserting. Negligible compared to the full-scan cost saved on the common case.
