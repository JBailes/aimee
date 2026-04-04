# Proposal: Fully On-Demand and Self-Healing Server Lifecycle

## Problem

The CLI startup path (`cli_client.c`) probes for an existing server socket, then
either connects or spawns a new server. Issues:

1. **Stale socket files.** If the server crashes without cleanup, the socket file
   remains. The CLI probes it (up to 200ms with the timeout fix from
   `cli-startup-improvements.md`), gets no response, and must fall back to
   spawning — but the stale socket file may block `bind()` on the new server.
2. **No health check retry.** If the server is mid-startup when the CLI connects,
   the connection may succeed but the server may not be ready to handle requests.
   There is no readiness check or retry.
3. **First-run friction.** On first invocation, the server must be started. If
   startup fails (e.g., port conflict, missing dependency), the error message is
   generic.

## Goals

- `aimee` cold-start succeeds with no manual daemon intervention.
- Stale socket files are detected and cleaned up automatically.
- Server readiness is verified before dispatching requests.

## Approach

### 1. Stale socket detection

Before connecting to an existing socket, check if a server process is actually
listening:

```c
/* Check if socket has a live server */
static int socket_is_live(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);

    if (rc == 0 || errno == EINPROGRESS) return 1;  /* live */
    if (errno == ECONNREFUSED) return 0;  /* stale */
    return -1;  /* error */
}
```

If stale: `unlink()` the socket file and proceed to spawn a new server.

### 2. Readiness handshake

After spawning a server, the CLI should verify readiness before dispatching:

```c
/* Wait for server readiness with bounded retry */
static int wait_for_ready(const char *socket_path, int timeout_ms) {
    int elapsed = 0;
    int backoff = 10;  /* start at 10ms */

    while (elapsed < timeout_ms) {
        int fd = try_connect(socket_path);
        if (fd >= 0) {
            /* Send hello/ping, verify response */
            if (send_ping(fd) == 0) { close(fd); return 0; }
            close(fd);
        }
        usleep(backoff * 1000);
        elapsed += backoff;
        backoff = (backoff < 200) ? backoff * 2 : 200;  /* cap at 200ms */
    }
    return -1;
}
```

Exponential backoff: 10ms → 20ms → 40ms → ... → 200ms cap. Total timeout: 3s.

### 3. Clear first-run diagnostics

If server spawn fails, produce specific error messages:

- Socket path not writable: "Cannot create socket at <path>: <errno>"
- Port conflict: "Another process holds <socket_path>"
- Missing database: "Database not initialized — run `aimee init`"

### 4. Lockfile for spawn coordination

Add a lockfile (`~/.config/aimee/server.lock`) to prevent multiple CLI
invocations from racing to spawn servers simultaneously. Use `flock()` with
`LOCK_NB` — if lock is held, wait for the other spawner to finish, then connect.

### Changes

| File | Change |
|------|--------|
| `src/cli_client.c` | Add stale socket detection, readiness handshake, spawn lockfile |
| `src/cli_launch.c` | Improved error messages for spawn failures |
| `src/server_main.c` | Clean up socket file on graceful shutdown (already exists, verify crash path) |

## Acceptance Criteria

- [ ] Stale socket files are detected and removed before spawn
- [ ] CLI waits for server readiness with exponential backoff (max 3s)
- [ ] Concurrent CLI invocations coordinate via lockfile (no double-spawn)
- [ ] Spawn failures produce specific, actionable error messages
- [ ] `aimee` cold-start succeeds reliably from clean state

## Owner and Effort

- **Owner:** TBD
- **Effort:** S-M
- **Dependencies:** CLI startup improvements proposal (for timeout reduction)

## Rollout and Rollback

- **Rollout:** Ships with next binary. Automatic behavior change.
- **Rollback:** Revert commit. Restores current probe-and-spawn behavior.
- **Blast radius:** If stale detection is wrong (live socket misidentified as stale), the server gets killed and restarted. Data loss is prevented by WAL mode.

## Test Plan

- [ ] Unit test: `socket_is_live()` correctly identifies live vs stale sockets
- [ ] Integration test: kill server, verify CLI auto-cleans socket and respawns
- [ ] Integration test: concurrent `aimee` invocations — only one spawns server
- [ ] Integration test: server mid-startup — CLI waits for readiness
- [ ] Manual: first-run from clean state succeeds without errors

## Operational Impact

- **Metrics:** None.
- **Logging:** Log stale socket cleanup and spawn coordination events.
- **Alerts:** None.
- **Disk/CPU/Memory:** Lockfile adds one file. Readiness retries add up to 3s worst-case startup.

## Priority

P2 — improves first-run and crash-recovery experience.

## Trade-offs

**Why lockfile instead of PID file?** PID files can become stale (PID reuse). A
lockfile with `flock()` is automatically released when the process exits, even on
crash.

**Why 3s total readiness timeout?** Long enough for server to initialize database
and bind socket. Short enough that users don't perceive a hang.
