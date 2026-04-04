# Proposal: Windows OS Support

## Problem

Aimee is currently Linux/macOS only. The codebase has `AIMEE_WINDOWS` detection in `platform.h` and stub implementations in the four platform abstraction files, but every stub returns `-1` (not implemented). Beyond the platform layer, dozens of source files use POSIX APIs directly (`fork`, `pipe`, `unistd.h`, `sys/wait.h`, `sys/un.h`, `signal.h`, `poll.h`, `fcntl.h`, `regex.h`, `chmod`, `umask`, `getppid`, `AF_UNIX`, `SO_PEERCRED`, etc.) without `#ifdef` guards. The build system is GNU Make with GCC, the installer is a bash script, the service manager integration is systemd, and all path logic assumes `$HOME/.config/aimee/` with forward slashes.

Users on Windows cannot build, install, or run aimee today.

## Goals

- `aimee` (client) and `aimee-server` compile and run natively on Windows 10+ (x64) without WSL.
- All core functionality works: memory, index, rules, config, agent delegation, MCP server, git operations, secret storage.
- The server runs as a background process (Windows Service or auto-start task) equivalent to the current systemd unit.
- Install experience is a single script or executable (PowerShell or MSI).
- Linux and macOS remain the primary development platforms with no regressions.

## Approach

The work divides into six phases. Each phase is independently shippable and testable.

### Phase 1: Build System (Effort: S)

Add MSVC and MinGW-w64 build support alongside the existing GCC Makefile.

| File | Change |
|------|--------|
| `src/Makefile` | Add `ifeq ($(OS),Windows_NT)` blocks for `.exe` suffixes, `NUL` instead of `/dev/null`, `del` instead of `rm`, Windows lib flags (`-lws2_32 -lbcrypt -lwinhttp`). Skip PAM and libsecret detection. |
| `CMakeLists.txt` (new) | Add CMake as the primary cross-platform build system. The Makefile stays for quick Linux dev builds. CMake handles MSVC, MinGW, and cross-compilation. |
| `src/vendor/` | Vendor `sqlite3.c` amalgamation so Windows builds don't need a separate sqlite3 install. |

### Phase 2: Platform Layer Implementation (Effort: L)

Implement the four `AIMEE_WINDOWS` stubs that already exist, plus add new platform abstractions for APIs used directly in non-platform files.

| File | Change |
|------|--------|
| `src/platform_process.c` | Implement `platform_spawn_daemon` via `CreateProcess` with `DETACHED_PROCESS` flag. Implement `platform_exec_capture` via `CreateProcess` + anonymous pipes + `WaitForSingleObject` timeout. |
| `src/platform_event.c` | Implement event loop via `WSAPoll` (for socket FDs) or `IOCP` (for better scalability). `WSAPoll` is simpler and matches the current epoll/kqueue abstraction well. |
| `src/platform_ipc.c` | Implement IPC via Windows Named Pipes (`CreateNamedPipe`, `ConnectNamedPipe`, `CreateFile`). Path translation: `~/.config/aimee/aimee.sock` becomes `\\.\pipe\aimee-<username>`. `platform_ipc_peer_cred` uses `GetNamedPipeClientProcessId` + `OpenProcessToken` for UID-equivalent checks. |
| `src/platform_random.c` | Already implemented (BCrypt). No changes needed. |
| `src/headers/platform.h` | Add `platform_fd_t` already done. Add `platform_getppid()` wrapper (Windows: `NtQueryInformationProcess` or toolhelp snapshot). |
| `src/headers/platform_path.h` (new) | New abstraction: `platform_home_dir()` (returns `%USERPROFILE%` on Windows, `$HOME` on POSIX), `platform_config_dir()` (returns `%APPDATA%\aimee` on Windows, `~/.config/aimee` on POSIX), `platform_path_sep()`, `platform_mkdir_p()`. |
| `src/headers/platform_signal.h` (new) | Wrapper for `signal()` / `SetConsoleCtrlHandler`. Map SIGTERM/SIGINT to console control events. |

### Phase 3: Source File Portability (Effort: L)

Audit and fix every `.c` file that uses POSIX APIs directly outside the platform layer.

**Category A: Replace direct POSIX calls with platform abstractions**

| Call | Replacement | Files affected |
|------|------------|----------------|
| `getenv("HOME")` | `platform_home_dir()` | config.c, db.c, cli_main.c, cli_client.c, client_integrations.c, dashboard.c, guardrails.c (14 call sites) |
| `getppid()` | `platform_getppid()` | config.c (2 call sites) |
| `fork()` + `exec*()` | `platform_exec_capture()` or `platform_spawn_daemon()` | agent_tools.c, mcp_git.c, worktree.c, cmd_hooks.c, git_verify.c |
| `pipe()` + `read()`/`write()` on pipes | Already behind `platform_exec_capture` except in agent_tunnel.c, server_compute.c, server_forward.c | 3 files need refactoring |
| `chmod()` / `umask()` | `platform_set_permissions()` wrapper (no-op or ACL-based on Windows) | secret_store.c, log.c, platform_ipc.c |
| `unlink()` | `_unlink()` on Windows (or `platform_unlink()` wrapper) | secret_store.c, platform_ipc.c, worktree.c |
| `symlink()` / `lstat()` / `S_ISLNK` | Conditional compile or `platform_is_symlink()` | platform_ipc.c |
| `regex.h` (`regcomp`/`regexec`) | Use PCRE2 or a vendored regex lib (Windows has no POSIX regex) | util.c, guardrails.c |
| `poll.h` / `poll()` | `WSAPoll()` on Windows (via platform_event or inline `#ifdef`) | agent_tunnel.c, platform_ipc.c |
| `sys/wait.h` / `waitpid()` | `WaitForSingleObject` + `GetExitCodeProcess` via platform wrappers | util.c, worktree.c, agent_tunnel.c |
| `pthread_mutex_t` | `SRWLOCK` or `CRITICAL_SECTION` on Windows (or use C11 `mtx_t` with a compat shim) | log.c, memory.c, db.c, agent.c, compute_pool.c, server_compute.c, mcp_server.c, webchat.c |
| `signal(SIGTERM, ...)` | `SetConsoleCtrlHandler` | server_main.c, main.c |
| `gmtime_r()` | `gmtime_s()` on Windows (note: argument order is reversed) | util.c |

**Category B: Path separator handling**

All path construction (`snprintf(..., "%s/%s", ...)`) needs to work with both `/` and `\`. Approach: normalize all internal paths to forward slashes (Windows APIs accept both), only convert to backslash for display or when calling Windows-specific APIs.

**Category C: Socket vs Named Pipe in non-platform code**

Several files outside `platform_ipc.c` use raw socket calls (`AF_UNIX`, `sockaddr_un`, `send()`, `recv()`). These need to go through the platform IPC layer or use a thin compatibility shim.

| File | Issue |
|------|-------|
| `agent_tunnel.c` | Uses `pipe()`, `poll()`, `fork()`, `kill()`, `waitpid()` for SSH tunnel management. Needs full rewrite to use `CreateProcess` + overlapped I/O. |
| `agent_tools.c` | Uses `AF_UNIX` socket directly for tool execution. Route through `platform_ipc`. |
| `server_compute.c` | Uses `pipe()`, `fork()`, raw FD passing. Needs `CreateProcess` + named pipe equivalent. |
| `server_forward.c` | Uses `pipe()`, `fork()` for request forwarding. Same treatment. |
| `cli_client.c` | Direct `AF_UNIX` connect to server. Route through `platform_ipc_connect`. |
| `cmd_chat.c` | Direct `AF_UNIX` connect. Route through `platform_ipc_connect`. |

### Phase 4: Secret Store Backend (Effort: S)

| File | Change |
|------|--------|
| `src/secret_store.c` | Add Windows Credential Manager backend via `CredWrite`/`CredRead`/`CredDelete` (wincred.h). Slot into `detect_backend()` alongside Keychain and libsecret. |

### Phase 5: Service Management & Installer (Effort: M)

| File | Change |
|------|--------|
| `install.ps1` (new) | PowerShell installer: check prerequisites (Git, sqlite3, compiler), build via CMake, copy binaries to `%LOCALAPPDATA%\aimee\bin`, add to PATH, run `aimee init`, configure hooks. |
| `service/aimee-server.xml` (new) | [WinSW](https://github.com/winsw/winsw) or `sc.exe` service wrapper config for `aimee-server`. Alternative: scheduled task with `schtasks` that starts at login. |
| `src/server_main.c` | Add `--service` flag that calls `StartServiceCtrlDispatcher` for Windows Service mode. Keep existing foreground mode as default. |
| `src/configure-hooks.sh` | Refactor hook detection into a shared library. Add `configure-hooks.ps1` for Windows that handles Claude Code, Codex CLI, etc. on Windows paths (`%APPDATA%`). |

### Phase 6: CI & Testing (Effort: M)

| File | Change |
|------|--------|
| `.github/workflows/ci.yml` | Add Windows runner (`windows-latest`) with MSVC and MinGW builds. Run unit tests on Windows. |
| `src/tests/` | Audit all test files for POSIX assumptions. Key issue: tests that use `mkstemp`, `setenv`, `/tmp`, `popen`. Add `platform_tmpdir()` and `platform_setenv()` wrappers. |
| `src/tests/test_integration.sh` | Port to PowerShell (`test_integration.ps1`) or make the bash version WSL-compatible. |

## Acceptance Criteria

- [ ] `aimee version` prints version on Windows 10+ x64 (native, not WSL)
- [ ] `aimee init` creates database at `%APPDATA%\aimee\aimee.db`
- [ ] `aimee-server` starts, accepts connections via named pipe, serves MCP requests
- [ ] `aimee memory store/search/list` works
- [ ] `aimee index scan/find` works (requires universal-ctags on PATH)
- [ ] `aimee delegate` spawns a subprocess and captures output
- [ ] Secret store uses Windows Credential Manager when available, file fallback otherwise
- [ ] All existing unit tests pass on Linux and macOS (no regressions)
- [ ] Unit tests pass on Windows CI runner
- [ ] Integration tests pass on Windows (PowerShell variant)
- [ ] Install script works on a fresh Windows 10 machine with Git and a C compiler

## Owner and Effort

- **Owner:** TBD
- **Effort:** XL (estimated 3-4 weeks of focused work across all phases)
- **Dependencies:**
  - Phase 2 (platform layer) is the critical path; everything else depends on it
  - CMake adoption (Phase 1) unblocks Windows CI immediately
  - Phases 4-5 can be done in parallel with Phase 3

## Rollout and Rollback

- **Rollout:** Phased. Each phase merges to `testing` independently. Phase 1+2 enable "it compiles" on Windows. Phase 3 enables "it runs." Phases 4-6 bring it to parity.
- **Rollback:** Any phase can be reverted independently. Windows-specific code is behind `#ifdef AIMEE_WINDOWS` or in separate files (`install.ps1`, `configure-hooks.ps1`).
- **Blast radius:** Low for Linux/macOS. Platform abstractions replace direct calls but the POSIX implementations are unchanged. Risk is introducing subtle behavioral differences (path handling, signal semantics, pipe buffering).

## Test Plan

- [ ] Unit tests: all existing tests pass on all three platforms
- [ ] Unit tests: new tests for platform_ipc (named pipe), platform_process (CreateProcess), platform_event (WSAPoll), platform_path (Windows path resolution)
- [ ] Integration tests: client-server roundtrip on Windows via named pipes
- [ ] Integration tests: agent delegation spawns and captures on Windows
- [ ] Failure injection: named pipe path already in use, server killed mid-request, disk full on `%APPDATA%`
- [ ] Manual verification: install on fresh Windows 10 VM, run full workflow (init, setup, memory store, index scan, delegate)

## Operational Impact

- **Metrics:** No new metrics. Existing counters work cross-platform.
- **Logging:** No changes. Log paths resolve via `platform_config_dir()`.
- **Alerts:** N/A (aimee doesn't have an alerting system).
- **Disk/CPU/Memory:** Named pipes have slightly different buffering than Unix sockets. No measurable impact expected.

## Priority

| Phase | Priority | Effort | Impact |
|-------|----------|--------|--------|
| 1. Build system (CMake) | P2 | S | Enables all subsequent phases |
| 2. Platform layer impl | P1 | L | Core enabler, nothing works without it |
| 3. Source portability | P1 | L | Makes it actually run |
| 4. Secret store | P3 | S | Nice-to-have, file fallback works |
| 5. Service & installer | P2 | M | Required for real-world use |
| 6. CI & testing | P1 | M | Prevents regressions |

## Trade-offs

**CMake vs Makefile-only:** Adding CMake is extra build system complexity. Considered keeping Makefile-only with MinGW, but MSVC support matters for Windows developers and CMake is the de facto standard for cross-platform C. The Makefile remains for quick Linux builds.

**WSAPoll vs IOCP:** `WSAPoll` is simpler and maps 1:1 to the current epoll/kqueue abstraction. IOCP would be more scalable but requires a fundamentally different (completion-based vs readiness-based) event model. Given aimee-server handles ~10 concurrent connections max, WSAPoll is sufficient. Can migrate to IOCP later if needed.

**Named Pipes vs AF_UNIX on Windows:** Windows 10 1803+ supports `AF_UNIX` sockets. Considered using them for code simplicity, but Named Pipes are the idiomatic Windows IPC, have better tooling support, and work on older Windows 10 builds. Named Pipes also provide built-in security descriptors for access control (replacing `chmod 0600` on the socket file).

**PCRE2 vs vendored regex:** `regex.h` (POSIX regex) doesn't exist on MSVC. Options: (a) vendor a small regex lib like [tiny-regex-c](https://github.com/kokke/tiny-regex-c), (b) use PCRE2 as an optional dependency, (c) replace regex usage with manual parsing. Recommend (b) since regex usage is limited to 2 files and PCRE2 is widely available via vcpkg/conan.

**pthreads vs Windows threads:** Could use a pthreads-win32 shim, but that's a heavy dependency for basic mutex/thread usage. Recommend a thin `platform_mutex_t` / `platform_thread_t` abstraction since the threading API surface is small (init, lock, unlock, destroy, create, join).
