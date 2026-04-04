# Proposal: Non-Blocking Server Writes with Per-Connection Output Buffers

## Problem

`conn_send()` (`server.c:48-63`) performs synchronous blocking writes with EINTR
retry loops. Since the server uses a single-threaded epoll event loop
(`server_run()` at `server.c:541-607`), a slow or stalled client blocks all
other connections until the write completes or the kernel buffer fills. With 64
max connections (`SERVER_MAX_CONNECTIONS`), one misbehaving client can stall the
entire server.

Each connection already has a 256KB write buffer (`SERVER_WRITE_BUF_SIZE =
262144`, `server.h`), but it is used as a staging area before the blocking
write, not as a persistent output queue.

## Goals

- One slow client cannot stall event-loop progress for other clients.
- Write failures are bounded by deadline, not by kernel buffer behavior.
- Connections that cannot drain their output buffer within a deadline are closed.

## Approach

### 1. Switch sockets to non-blocking mode

`accept_connection()` (`server.c:305-354`) already uses `accept4()` with
`SOCK_NONBLOCK`. Verify all subsequent `write()` callers handle `EAGAIN`/
`EWOULDBLOCK`.

### 2. Queue unsent bytes in per-connection ring buffer

Replace the current flat write buffer with a ring buffer (or reuse the existing
256KB buffer as a circular queue with head/tail pointers). When `write()` returns
`EAGAIN`, stash remaining bytes in the buffer instead of retrying.

```c
typedef struct {
    char buf[SERVER_WRITE_BUF_SIZE];
    size_t head;      /* next byte to write */
    size_t tail;      /* next byte to fill */
    size_t pending;   /* tail - head */
} conn_outbuf_t;
```

### 3. Drive flushes on EPOLLOUT

Register `EPOLLOUT` interest when a connection has pending output bytes. In the
event loop, when `EPOLLOUT` fires, call a new `conn_flush()` that drains as much
as the kernel will accept. Remove `EPOLLOUT` interest when the buffer is empty.

### 4. Idle write deadline

Add a per-connection write deadline (`CONN_WRITE_DEADLINE_MS = 10000`). If a
connection has had pending output for longer than the deadline, close it with a
log message. Check deadlines on each event-loop iteration (cheap — just compare
`clock_gettime` against `conn->write_deadline`).

### Changes

| File | Change |
|------|--------|
| `src/server.c` | Replace `conn_send()` blocking loop with non-blocking write + queue, add `conn_flush()`, register/deregister `EPOLLOUT` |
| `src/server.c` | Add write deadline check in `server_run()` loop |
| `src/server.h` | Add `conn_outbuf_t` fields and `CONN_WRITE_DEADLINE_MS` constant to `server_conn_t` |

## Acceptance Criteria

- [ ] `conn_send()` never blocks the event loop — returns immediately after queuing
- [ ] Connections with pending output are flushed via `EPOLLOUT`
- [ ] Connections exceeding write deadline are closed with log message
- [ ] All existing server functionality works unchanged under normal conditions
- [ ] Load test: 64 concurrent connections, one artificially throttled to 1 byte/sec, others unaffected

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Ships with next binary build. No config changes needed.
- **Rollback:** Revert commit. Restores blocking write behavior.
- **Blast radius:** All server connections. If the ring buffer logic has bugs, connections may see truncated or corrupted responses.

## Test Plan

- [ ] Unit test: `conn_outbuf_t` ring buffer operations (wrap-around, full, empty)
- [ ] Unit test: `conn_flush()` drains correct bytes in correct order
- [ ] Integration test: slow client (artificial `SO_RCVBUF` limit) does not block fast clients
- [ ] Integration test: client that stops reading is disconnected after deadline
- [ ] Failure injection: `write()` that always returns `EAGAIN` — verify buffer fills and deadline fires

## Operational Impact

- **Metrics:** None new (future: per-connection pending bytes counter).
- **Logging:** New log line when connection closed due to write deadline.
- **Alerts:** None.
- **Disk/CPU/Memory:** No change in steady state. Worst case: 64 connections × 256KB = 16MB output buffer memory (same as current allocation, just used differently).

## Priority

P0 — blocking writes are the primary scalability bottleneck in the event loop.

## Trade-offs

**Why not use writev/scatter-gather?** The ring buffer approach is simpler and
avoids managing iovec arrays. If profiling shows syscall overhead matters,
`writev` can be added later as an optimization within `conn_flush()`.

**Why 10-second write deadline?** Long enough for transient network hiccups
(especially over SSH tunnels), short enough to prevent indefinite resource
holding. Configurable via server config if needed later.
