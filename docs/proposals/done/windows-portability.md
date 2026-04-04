# Proposal: Windows and macOS Portability Strategy

## Problem

aimee is built on Linux-specific APIs throughout the codebase:

- **IPC:** Unix domain sockets (`AF_UNIX`) for server communication
- **Process model:** `fork()` for background server spawn and prune children
- **Event loop:** `epoll` for server I/O multiplexing
- **Auth:** `SO_PEERCRED` for peer credential extraction
- **File ops:** `/dev/urandom`, `flock()`, symlink handling
- **Webchat:** PAM for authentication

The codebase has dormant Windows-specific TODOs (WinHTTP `extra_headers`, form
POST) in the HTTP client. This partial state confuses contributors about what
is supported.

## Goals

- Define an explicit portability target: Linux (primary), macOS (secondary), Windows (tertiary).
- Introduce abstraction layers for platform-divergent APIs.
- Ship macOS support as a concrete near-term target.
- Ship Windows support as a concrete medium-term target.
- Remove or complete dormant Windows code paths.

## Approach

### 1. Platform abstraction layers

Create thin abstraction headers that select the right implementation at compile
time:

```c
/* platform_ipc.h */
#if defined(_WIN32)
    /* Named pipes */
    typedef HANDLE ipc_socket_t;
    int ipc_listen(const char *name, ipc_socket_t *out);
    int ipc_connect(const char *name, ipc_socket_t *out);
#else
    /* Unix domain sockets */
    typedef int ipc_socket_t;
    int ipc_listen(const char *path, ipc_socket_t *out);
    int ipc_connect(const char *path, ipc_socket_t *out);
#endif
```

Layers needed:

| Layer | Linux | macOS | Windows |
|-------|-------|-------|---------|
| IPC | Unix socket | Unix socket | Named pipes |
| Event loop | epoll | kqueue | IOCP / WSAPoll |
| Process spawn | fork+exec | fork+exec | CreateProcess |
| Peer auth | SO_PEERCRED | LOCAL_PEERCRED | Named pipe GetNamedPipeClientProcessId |
| Random | /dev/urandom | /dev/urandom | BCryptGenRandom |
| File lock | flock() | flock() | LockFileEx |
| Webchat auth | PAM | PAM (macOS) | SSPI (optional) |

### 2. macOS support (Phase 1)

macOS shares most POSIX APIs with Linux. Key differences:
- `epoll` → `kqueue` (or use `poll()` as portable fallback for small connection counts)
- `SO_PEERCRED` → `LOCAL_PEERCRED` / `getpeereid()`
- PAM is available on macOS

Effort is primarily event loop abstraction and peer credential extraction.

### 3. Windows support (Phase 2)

Major changes:
- Named pipes replace Unix sockets (path format: `\\.\pipe\aimee-<user>`)
- `CreateProcess` replaces `fork()`
- IOCP or `WSAPoll` replaces `epoll`
- `BCryptGenRandom` replaces `/dev/urandom`
- `LockFileEx` replaces `flock()`

### 4. Clean up dormant code

Remove or implement incomplete Windows paths in the HTTP client. If implementing,
add CI coverage. If removing, add a clear comment pointing to this proposal.

### 5. CI matrix

Add GitHub Actions CI matrix:
- Linux x86_64 (primary, existing)
- macOS arm64 (Phase 1)
- Windows x86_64 (Phase 2)

### Changes

| File | Change |
|------|--------|
| `src/platform_ipc.h/c` | New: IPC abstraction (Unix sockets / named pipes) |
| `src/platform_event.h/c` | New: Event loop abstraction (epoll / kqueue / IOCP) |
| `src/platform_process.h/c` | New: Process spawn abstraction (fork / CreateProcess) |
| `src/platform_auth.h/c` | New: Peer credential abstraction |
| `src/platform_random.h/c` | New: Cryptographic random abstraction |
| `src/server.c` | Use platform abstractions instead of direct Linux APIs |
| `src/cli_client.c` | Use platform IPC abstraction |
| `src/agent_http.c` | Complete or remove Windows HTTP TODOs |
| `.github/workflows/` | Add macOS and Windows CI targets |

## Acceptance Criteria

- [ ] Platform abstraction headers compile on Linux, macOS, and Windows
- [ ] Server runs on macOS with kqueue event loop (Phase 1)
- [ ] Server runs on Windows with named pipes and IOCP (Phase 2)
- [ ] All tests pass on all three platforms in CI
- [ ] No dormant platform-specific TODOs without tracking issue

## Owner and Effort

- **Owner:** TBD
- **Effort:** L (Phase 1: M for macOS, Phase 2: L for Windows)
- **Dependencies:** None (but benefits from all other proposals using abstractions)

## Rollout and Rollback

- **Rollout:** Phase 1 (macOS) and Phase 2 (Windows) are independent releases.
- **Rollback:** Platform-specific code is behind compile-time switches. Revert does not affect Linux.
- **Blast radius:** Abstraction layer introduction touches most server code paths. Thorough testing on Linux is critical to avoid regressions on the primary platform.

## Test Plan

- [ ] Unit test: platform abstraction APIs on each target platform
- [ ] Integration test: full server lifecycle on macOS (Phase 1)
- [ ] Integration test: full server lifecycle on Windows (Phase 2)
- [ ] CI: all existing tests pass on all platforms
- [ ] Manual: `aimee` cold-start on fresh macOS and Windows installs

## Operational Impact

- **Metrics:** None.
- **Logging:** Platform detected and logged at startup.
- **Alerts:** None.
- **Disk/CPU/Memory:** Platform abstractions add one function-call indirection. Negligible.

## Priority

P1 — macOS support is a near-term target that expands the user base significantly
and is the longest-lead item in the roadmap. Windows is medium-term. Starting
the abstraction layer early unblocks other cross-platform work (OS keyring,
platform-specific CI).

## Trade-offs

**Why not use a cross-platform library (libuv, libevent)?** These are large
dependencies for a tool that needs only basic IPC, events, and process management.
Thin hand-written abstractions keep the dependency footprint small and the code
auditable.

**Why kqueue for macOS instead of poll()?** `poll()` works but scales poorly
beyond ~100 fds. kqueue is the native macOS event API and matches epoll's
performance characteristics. Since we already need an abstraction layer, using
the native API is worth the effort.

**Why not WSL for Windows?** WSL is a viable deployment target but not a native
one. Many Windows users do not have WSL configured. Native support via named
pipes is the expected experience.
