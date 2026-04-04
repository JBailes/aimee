# Proposal: Persistent Server by Default with Session-Aware Idle Shutdown

## Problem

The auto-spawned server is ephemeral: it listens on a per-PID socket
(`aimee-<pid>.sock`) and checks `getppid()` every 5 seconds, shutting down when
the spawning CLI exits (`server.c:698-704`). This causes three issues:

1. **Server dies between CLI invocations.** Every `aimee delegate` or `aimee
   memory` call spawns a fresh server, paying startup cost (~100ms) and losing
   in-memory state (SQLite page cache, compute pool).
2. **Per-PID socket is unfindable.** The next CLI invocation has no way to
   discover the previous server's socket. `cli_ensure_server()` checks the
   well-known path (`aimee.sock`) first, but the ephemeral server never listens
   there.
3. **Stale sockets accumulate.** Crashed servers leave `aimee-<pid>.sock` files
   on disk indefinitely.

The `--persistent` flag exists but must be started manually. Most users never
run it, so they hit the ephemeral path by default.

## Goals

- Auto-spawned server listens on the well-known socket and survives CLI exits.
- Server shuts itself down after 30 minutes of no active sessions.
- `--persistent` mode disables idle shutdown entirely (for systemd/long-lived).
- No stale per-PID sockets left on disk.
- Users who need ephemeral behavior can still get it via environment variable.

## Approach

### 1. Spawn on well-known socket

In `spawn_server()` (`cli_client.c:366`), change the socket path from
`aimee-<pid>.sock` to `cli_default_socket_path()` (`aimee.sock`). The spawn
lock already prevents concurrent double-spawn.

```c
/* Before */
snprintf(sock_path, sizeof(sock_path), "%s/aimee-%d.sock",
         cli_config_dir(), (int)getpid());

/* After */
snprintf(sock_path, sizeof(sock_path), "%s", cli_default_socket_path());
```

### 2. Drop parent-PID tracking

Remove the `getppid()` check from `server_run()` (`server.c:698-704`). The
server no longer has a meaningful parent relationship since it outlives the
spawning CLI.

```c
/* Remove this block: */
if (parent_pid > 1 && getppid() != parent_pid)
{
   fprintf(stderr, "aimee-server: parent process gone, shutting down\n");
   ctx->running = 0;
}
```

Also remove the `parent_pid` variable and `last_parent_check` timer since they
are no longer used.

### 3. Session-aware idle timeout

The existing idle timeout logic (`server.c:706-711`) already tracks
`last_session_end`. Keep this, but:

- Raise `SERVER_IDLE_TIMEOUT` from 300 (5 min) to 1800 (30 min) in `server.h`
- Only apply idle timeout when `ctx->persistent == 0` (already gated by the
  `!ctx->persistent` check)
- `--persistent` mode skips the entire lifecycle check block, so it never idles

The idle timeout starts counting only after all sessions have closed
(`active_sessions` drops to 0, which sets `last_session_end`). While any
session is open, the server stays alive indefinitely.

### 4. Remove AIMEE_SOCK override from spawn path

Since `spawn_server()` now uses the well-known socket, remove the
`setenv("AIMEE_SOCK", spawned, 1)` call in `cli_ensure_server()`
(`cli_client.c:519`). The well-known socket is already checked first in
`cli_ensure_server()`, so no environment variable is needed.

`AIMEE_SOCK` remains honored if set externally (for testing, CI, or explicit
ephemeral use).

### 5. Update spawn comment and function name

Rename `spawn_server()` comment from "non-persistent" to reflect the new
behavior. The function now spawns a server that outlives its parent but
respects idle timeout.

### Changes

| File | Change |
|------|--------|
| `src/cli_client.c` | `spawn_server()`: use well-known socket path, remove `setenv` |
| `src/server.c` | Remove `getppid()` check and `parent_pid`/`last_parent_check` variables |
| `src/headers/server.h` | Change `SERVER_IDLE_TIMEOUT` from 300 to 1800 |

## Acceptance Criteria

- [ ] `aimee delegate draft "hello"` works without manually starting a server
- [ ] Server spawns on `~/.config/aimee/aimee.sock`, not `aimee-<pid>.sock`
- [ ] Server survives the spawning CLI process exiting
- [ ] Server shuts down after 30 minutes with no active sessions
- [ ] `--persistent` mode never shuts down due to idle timeout
- [ ] `AIMEE_SOCK` still works for explicit ephemeral/testing use
- [ ] No per-PID socket files created during normal operation

## Owner and Effort

- **Owner:** aimee
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Ships with next binary build. Existing ephemeral servers die
  naturally. New server spawns on well-known socket on first CLI invocation.
- **Rollback:** Revert commit. Restores ephemeral per-PID behavior.
- **Blast radius:** All server auto-start behavior. If the idle timeout is too
  aggressive, users may see cold-start latency after 30 minutes of inactivity.
  If too lenient, servers may linger longer than desired on resource-constrained
  systems.

## Test Plan

- [ ] Unit test: server shuts down after idle timeout with no active sessions
- [ ] Unit test: server does NOT shut down while sessions are active
- [ ] Unit test: `--persistent` server ignores idle timeout
- [ ] Integration test: spawn server via CLI, exit CLI, run CLI again, verify same server
- [ ] Integration test: close all sessions, wait >30 min, verify server exits
- [ ] Manual verification: `ls ~/.config/aimee/aimee*.sock` shows only `aimee.sock`

## Operational Impact

- **Metrics:** Server uptime increases (no longer restarting per-CLI-invocation).
- **Logging:** "parent process gone" log line removed. Idle shutdown log line
  remains.
- **Alerts:** None.
- **Disk/CPU/Memory:** Server process persists ~30 min after last session. Memory
  footprint is ~30MB (SQLite cache + compute pool). Acceptable for developer
  machines.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Well-known socket spawn | P0 | S | Fixes delegate reliability |
| Drop parent-PID check | P0 | S | Required for server to survive CLI exit |
| Raise idle timeout | P1 | S | Prevents premature shutdown |

## Trade-offs

**Why 30 minutes?** Long enough to cover a user reading docs or stepping away
briefly. Short enough that forgotten servers do not linger for hours. The idle
clock only starts after all sessions close, so active work is never interrupted.

**Why not just make everything persistent?** Persistent mode (no idle timeout)
is the right choice for systemd-managed deployments, but for developer machines
a server that lingers forever after the user is done working is wasteful.
Session-aware idle timeout gives the best of both: stays alive during work,
cleans up after.

**Why keep AIMEE_SOCK?** CI environments, test harnesses, and multi-workspace
setups may need isolated servers on separate sockets. Keeping `AIMEE_SOCK` as
an explicit override preserves that flexibility without complicating the default
path.
