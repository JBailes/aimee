# Proposal: Background Process Management for Agent Sessions

## Problem

When agents need to start long-running processes — dev servers, file watchers, build daemons, database migrations — they currently use one-shot shell commands that block until completion or timeout. There is no way to:
1. Start a process in the background and continue working
2. Check on a running process's output
3. Kill a background process that's no longer needed
4. Limit the number of concurrent background processes

ayder-cli implements a `ProcessManager` class with four tools (`run_background_process`, `get_background_output`, `kill_background_process`, `list_background_processes`) that manage background processes with:
- Output capture (stdout/stderr ring buffers, 500 lines each)
- Lifecycle tracking (running, exited, exit codes)
- Concurrent process limits (configurable, default 5)
- Automatic cleanup on session exit

This is directly useful for aimee's agent sessions — especially delegates that need to start a dev server, run tests against it, then shut it down.

Evidence:
- `agent_tools.c` has `exec_tool_bash()` which runs commands synchronously with a timeout
- Delegates running `npm start` or `make serve` block until timeout
- No way to run a build in background while editing files in parallel
- The webchat chat loop inherits the same limitation

## Goals

- Agents can start, monitor, and kill background processes during sessions.
- Background process output (stdout/stderr) is captured in ring buffers and retrievable on demand.
- A configurable limit prevents agents from spawning too many concurrent processes.
- Background processes are automatically cleaned up when the session ends.
- Tools are available in CLI chat, webchat, and delegate sessions.

## Approach

### 1. Process manager

Add `src/process_mgr.c` with a process table:

```c
#define PROC_MAX_CONCURRENT 5
#define PROC_OUTPUT_RING_SIZE 500  // lines per stream

typedef struct {
    int id;
    pid_t pid;
    char command[512];
    int status;           // PROC_RUNNING, PROC_EXITED
    int exit_code;
    time_t start_time;
    char *stdout_ring[PROC_OUTPUT_RING_SIZE];
    int stdout_head;
    char *stderr_ring[PROC_OUTPUT_RING_SIZE];
    int stderr_head;
    int stdout_pipe;
    int stderr_pipe;
} managed_process_t;

int proc_start(const char *command, const char *cwd);
int proc_get_output(int id, int tail_lines, char *out, size_t out_size);
int proc_kill(int id);
int proc_list(char *out, size_t out_size);
void proc_cleanup_all(void);
```

### 2. Background output capture

Use `pipe()` + `fork()` + non-blocking reads. A background thread polls all active process pipes and appends to their ring buffers.

### 3. MCP tools

Register four tools:

| Tool | Description | Permission |
|------|-------------|------------|
| `run_background_process` | Start a command in the background | execute |
| `get_background_output` | Get recent stdout/stderr from a process | read |
| `kill_background_process` | Kill a running process | execute |
| `list_background_processes` | List all processes and their status | read |

### 4. Webchat integration

In the webchat UI, add a "Processes" panel (similar to the existing delegation and traces panels) showing active background processes with their status and recent output.

### 5. CLI integration

In CLI chat and delegate sessions, background process tools appear in the tool set. Cleanup runs automatically via `atexit()`.

### Changes

| File | Change |
|------|--------|
| `src/process_mgr.c` (new) | Process table, start/kill/list/output, ring buffers, cleanup |
| `src/headers/process_mgr.h` (new) | Public process manager API |
| `src/mcp_tools.c` | Register 4 background process tools |
| `src/agent_tools.c` | Wire process tools into agent tool execution |
| `src/cmd_chat.c` | Initialize process manager, cleanup on exit |
| `src/webchat.c` | Initialize process manager, add `/api/processes` endpoint, cleanup on session end |
| `src/webchat_assets.c` | Add processes panel to dashboard HTML |
| `src/config.c` | Parse `max_background_processes` setting |

## Acceptance Criteria

- [ ] `run_background_process` starts a command and returns an ID
- [ ] `get_background_output` returns last N lines of stdout/stderr for a process
- [ ] `kill_background_process` terminates a running process
- [ ] `list_background_processes` shows all processes with status and PID
- [ ] Concurrent process limit is enforced (default 5)
- [ ] Processes are cleaned up on session exit
- [ ] Webchat shows process status in a dashboard panel
- [ ] Delegates can use process tools when `--tools` is enabled

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** M (3-4 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Additive — process tools appear in tool listings. No behavior change if not used.
- **Rollback:** Remove tool registrations. Process manager cleanup runs on exit regardless.
- **Blast radius:** A leaked background process could consume resources. Mitigation: automatic cleanup on session exit, concurrent limit, and `atexit()` handler.

## Test Plan

- [ ] Unit tests: start process, capture output, kill process, list processes, concurrent limit enforcement
- [ ] Integration tests: start `sleep 60`, verify it appears in list, kill it, verify exit
- [ ] Integration tests: start process, read output, verify ring buffer wrapping
- [ ] Failure injection: start process that crashes immediately, start more than max concurrent, kill non-existent ID
- [ ] Manual verification: delegate starts a dev server, runs tests against it, then kills it

## Operational Impact

- **Metrics:** `background_processes_started_total`, `background_processes_active`
- **Logging:** Process start/kill at INFO, output capture at DEBUG
- **Alerts:** `background_processes_active` > limit suggests leak
- **Disk/CPU/Memory:** Ring buffers use ~500 lines × ~200 bytes × 2 streams × 5 processes ≈ 1MB max. Background polling thread is lightweight.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Core process manager | P2 | M | High — enables dev server workflows |
| MCP tools | P2 | S | High — agent access |
| Webchat panel | P3 | S | Medium — visibility |
| Ring buffer output capture | P2 | S | High — without output, agents can't verify process state |

## Trade-offs

- **Why not just use `&` in shell commands?** Shell backgrounding doesn't provide output capture, lifecycle tracking, or cleanup guarantees. The process manager wraps these concerns into a reliable API.
- **Why ring buffers instead of full output?** Long-running processes (dev servers) produce unbounded output. Ring buffers cap memory usage while preserving the most recent (and most useful) output.
- **Why a concurrent limit?** Agents can accidentally spawn many processes. The limit prevents resource exhaustion on the host.

## Source Reference

Implementation reference: ayder-cli `process_manager.py` — `ProcessManager` class with `ManagedProcess` dataclass, ring buffers, thread-safe output capture, and automatic cleanup.
