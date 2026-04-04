# Proposal: On-Demand Server Lifecycle

## Problem

The aimee-server currently runs as a persistent daemon, requiring manual startup or systemd configuration. If a user types `aimee` and the server isn't running, they get `"cannot connect to aimee-server (is it running?)"` and nothing happens. The install script does not enable the systemd service, so first-run experience is broken for users who don't know to start the server manually.

The server also runs indefinitely even when no sessions are active, consuming resources unnecessarily on developer machines.

Additionally, non-persistent and persistent servers share a single socket path, meaning they cannot coexist. If a persistent server (systemd) crashes mid-session, a client-started replacement would block systemd from restarting on the same socket.

## Goals

- `aimee` (no args) works immediately after install — no manual server start required.
- `aimee <subcommand>` (e.g., `aimee memory search`) also auto-starts the server if needed.
- The server stops when the last session ends (via wrapup), unless running in persistent mode.
- Persistent (systemd) and non-persistent (session) servers can coexist without socket conflicts.
- If the server goes down mid-session, the client transparently starts a new one.

## Approach

### Socket separation

**Persistent servers** bind to the well-known socket: `~/.config/aimee/aimee.sock`. This path is stable, predictable, and owned by systemd.

**Non-persistent servers** bind to a unique socket: `~/.config/aimee/aimee-<pid>.sock` (using the server's PID for uniqueness). This ensures multiple concurrent sessions each get their own server without conflicting with each other or with a persistent server.

The socket path is communicated to the provider CLI (and its hooks) via the `AIMEE_SOCK` environment variable, set by `cli_main.c` before `execlp()`. Hooks and subcommands check `AIMEE_SOCK` first, then fall back to the well-known socket.

### Connection order

When a client needs the server, `cli_ensure_server()` follows this order:

1. If `AIMEE_SOCK` is set, try that socket with a short timeout (100ms). If up, use it.
2. Try the well-known socket `~/.config/aimee/aimee.sock`. If up, use it.
3. If neither is available, fork/exec a non-persistent `aimee-server` on a new random socket. Set `AIMEE_SOCK` to the new path. Wait for socket to appear (up to 3 seconds), retry.

Note: Step 1 uses a short timeout (100ms instead of the default 1s) because if the session server died, every hook call would pay the full connect timeout before falling through. With 100ms, the fallback path adds negligible latency.

This means:
- During a session, hooks use the session's own server (via `AIMEE_SOCK`)
- If that server dies, hooks fall back to the persistent server
- If neither exists, a new non-persistent server is started
- One-off subcommands (`aimee memory search`) outside a session try the persistent server, then auto-start a non-persistent one

### 1. Auto-start from client

Add `cli_ensure_server()` to `cli_client.c`. Called from `cli_main.c` before all server interactions: `launch_session()`, `forward_command()`, and `handle_hooks()`.

The child process calls `setsid()` and redirects stdio to `/dev/null` so the server outlives the spawning CLI process. The parent polls for the socket file with `stat()` before retrying connection.

In `cli_main.c`, after auto-starting a non-persistent server, set `AIMEE_SOCK` in the environment before `execlp()`ing the provider. This ensures all hooks and subcommands within the session route to the correct server.

### 2. Session refcounting + explicit shutdown at wrapup

Add `active_sessions` (atomic int) to `server_ctx_t`. The server detects `launch`, `session-start`, and `wrapup` commands flowing through `handle_cli_forward()` in `server_forward.c`:

- `launch` or `session-start`: atomically increment
- `wrapup`: atomically decrement (floor at 0)

Uses gcc `__atomic` builtins since the forward worker runs in the compute thread pool.

**Shutdown on last wrapup:** When the wrapup forward completes and `active_sessions` reaches 0 and `persistent == 0`, the server initiates graceful shutdown (`ctx->running = 0`). This is the primary shutdown mechanism — deterministic and tied to session lifecycle.

### 3. Safety-net idle timeout

A 5-minute idle timeout catches edge cases where wrapup never ran (crash, `kill -9`, etc.). In `server_run()`, after each `epoll_wait` iteration:

- If `active_sessions == 0 && persistent == 0` and 5 minutes have elapsed since the last session ended (or since startup if no session ever started): set `ctx->running = 0`.
- If `active_sessions > 0`: reset idle timer.

This is a safety net only. Normal shutdown happens via wrapup.

One-off subcommands like `aimee memory search` don't create sessions, so the server auto-starts, handles the request, and idles out after 5 minutes. This is acceptable since the startup cost is ~10ms.

### 4. Persistent mode (`--persistent`)

Add `--persistent` flag to `aimee-server`. When set:

- Server binds to the well-known socket `~/.config/aimee/aimee.sock`
- Wrapup shutdown is ignored (server stays running even when `active_sessions` hits 0)
- Idle timeout is disabled

Without `--persistent`, the server binds to a unique socket (`aimee-<pid>.sock`).

The `aimee-server.service` systemd unit uses `--persistent`. Systemd's `Restart=on-failure` provides the persistence guarantee. This is used by webchat and any future long-running deployments.

### 5. Socket ownership and permission enforcement

The server creates sockets with `chmod(sock_path, 0600)` and the enclosing directory (`~/.config/aimee/`) must be `0700`. This prevents other users on shared hosts from connecting to or impersonating the server.

**Server-side (creation):**
- `server_init()` already calls `chmod()` on the socket after `bind()`. Verify this is `0600`.
- Before binding, check that the parent directory is owned by `getuid()` and mode `0700`. If not, `fprintf(stderr, ...)` a warning and refuse to start. This prevents a TOCTOU attack where an attacker creates `~/.config/aimee/` as a symlink.

**Client-side (connection):**
- `cli_ensure_server()` must `stat()` the socket file before `connect()` and verify `st_uid == getuid()` and `(st_mode & 0077) == 0` (no group/other access). If validation fails, skip this socket and fall through to the next option (or fall back to in-process).
- This applies to both `AIMEE_SOCK` and the well-known socket path.

### 6. Server cleanup on shutdown

On graceful shutdown, the server unlinks its socket file (already implemented in `server_shutdown()`). This ensures stale sockets from non-persistent servers don't accumulate. The 5-minute idle timeout handles the case where a non-persistent server is orphaned.

### Changes

| File | Change |
|------|--------|
| `src/cli_client.c` | Add `cli_ensure_server()`: try `AIMEE_SOCK`, try well-known socket, fork/exec if neither available |
| `src/headers/cli_client.h` | Declare `cli_ensure_server()` |
| `src/cli_main.c` | Call `cli_ensure_server()` before all server interactions; set `AIMEE_SOCK` before `execlp()` |
| `src/headers/server.h` | Add `active_sessions`, `persistent`, `last_session_end` to `server_ctx_t` |
| `src/server_forward.c` | Detect launch/session-start/wrapup, update refcount; trigger shutdown on last wrapup (non-persistent only) |
| `src/server.c` | Add 5-minute safety-net idle timeout in `server_run()` |
| `src/server_main.c` | Add `--persistent` flag; persistent uses well-known socket, non-persistent uses `aimee-<pid>.sock` |
| `src/server.c` | Enforce `chmod(sock, 0600)` after bind; validate parent dir ownership and mode before binding |
| `src/cli_client.c` | Validate socket `st_uid == getuid()` and `(st_mode & 0077) == 0` before `connect()` |
| `systemd/aimee-server.service` | Add `--persistent` to ExecStart |

## Acceptance Criteria

- [ ] `aimee memory list` with no server running auto-starts a non-persistent server on a unique socket, executes the command; server idles out after 5 minutes
- [ ] `aimee` with no server running auto-starts server, sets `AIMEE_SOCK`, launches claude; hooks use `AIMEE_SOCK`; after wrapup, server shuts down
- [ ] Two concurrent `aimee` sessions each get their own non-persistent server on separate sockets
- [ ] `aimee-server --persistent` binds to well-known socket, ignores wrapup shutdown, never idle-exits
- [ ] Persistent server running: `aimee` uses it (via well-known socket fallback); wrapup does not kill it
- [ ] Persistent server crashes mid-session: hooks fall back to session server (via `AIMEE_SOCK`); systemd restarts persistent server on well-known socket without conflict
- [ ] Session server crashes mid-session: next hook call auto-starts a new non-persistent server
- [ ] No stale socket files accumulate (servers unlink on shutdown; idle timeout cleans up orphans)
- [ ] Server socket files are created with mode `0600` — no group/other access
- [ ] Server refuses to start if parent directory (`~/.config/aimee/`) is not owned by current user or has group/other write
- [ ] Client refuses to connect to a socket not owned by current user
- [ ] All existing unit tests pass

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (4-5 hours implementation + testing)
- **Dependencies:** PR #106 (launch command) must be merged first

## Rollout and Rollback

- **Rollout:** Normal binary update. Systemd service file updated with `--persistent`. New users get auto-start behavior immediately with no configuration.
- **Rollback:** Revert commit. Users restart their systemd service or manually run `aimee-server`.
- **Blast radius:** If auto-start fails, same behavior as today (error message). If shutdown-on-wrapup fails, safety-net timeout catches it within 5 minutes.

## Test Plan

- [ ] Unit tests: atomic refcount increment/decrement edge cases (overflow, underflow)
- [ ] Integration tests: auto-start with server not running, shutdown-on-wrapup, persistent flag ignoring shutdown, `AIMEE_SOCK` propagation
- [ ] Failure injection: server socket exists but process is dead (stale socket), two clients race to start server, session killed without wrapup, persistent server crash mid-session
- [ ] Failure injection: socket owned by different user — client skips it and falls through to next option
- [ ] Failure injection: parent directory with `0777` permissions — server refuses to bind
- [ ] Integration test: cold start, concurrent sessions on separate sockets, persistent + non-persistent coexistence

## Operational Impact

- **Metrics:** None new (server already has uptime in `server.health`).
- **Logging:** Server prints `"last session ended, shutting down"` on wrapup shutdown and `"idle timeout, shutting down"` on safety-net exit. Socket path logged at startup.
- **Alerts:** None.
- **Disk/CPU/Memory:** Reduced — server no longer runs 24/7 on dev machines. Brief startup cost (~10ms) on first use. Multiple concurrent sessions each run their own server (minimal overhead — each is ~1MB RSS).

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Socket separation + auto-start | P1 | M | Fixes first-run experience and coexistence |
| Wrapup shutdown + refcounting | P1 | S | Deterministic server lifecycle |
| Safety-net idle timeout | P2 | S | Crash resilience |
| `--persistent` flag + systemd | P2 | S | Supports webchat/systemd use case |

## Trade-offs

**Alternative: Single shared socket** — Both persistent and non-persistent servers share `aimee.sock`. Rejected: creates conflicts when systemd tries to restart after a client-started server took the socket. Socket separation eliminates this entire class of problems.

**Alternative: Idle timeout as primary shutdown** — Timeout after last session ends. Rejected: too fragile (too short risks mid-session shutdown; too long wastes resources). Explicit shutdown on wrapup is deterministic.

**Alternative: Connection-based session tracking** — Track sessions by holding connections open. Rejected: the CLI opens a connection per request and closes it; the actual session is managed by the provider CLI (claude), which doesn't hold an aimee-server connection.

**Alternative: PID file tracking** — Server writes PID file, client checks if alive before spawning. Rejected: socket probe is simpler and already handles stale processes (`server_init` detects stale sockets).
