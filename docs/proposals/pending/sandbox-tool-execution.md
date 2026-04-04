# Proposal: Sandboxed Tool Execution

## Problem

When aimee delegates tool execution to agents (or runs tools directly via MCP), commands execute with the full privileges of the aimee process. A misbehaving or manipulated agent can `rm -rf /`, write to arbitrary paths, exfiltrate data over the network, or corrupt aimee's own state. The `pre_tool_check` guardrail in `agent_policy.c` is a pattern-matching blocklist — it catches known-bad commands but cannot prevent novel attacks or subtle path traversals.

The claw-code project (github.com/ultraworkers/claw-code) implements Linux namespace-based sandboxing in its `runtime/sandbox.rs` module: user/mount/IPC/PID/UTS/network namespaces via `unshare`, with filesystem isolation modes (workspace-only, allowlist), redirected HOME/TMPDIR, and graceful degradation in container environments. This is a proven pattern that aimee should adopt.

Evidence of the gap:
- `platform_process.c` spawns child processes with `fork()/exec()` — no isolation
- `agent_tools.c` invokes bash commands directly via `popen()` or `system()`
- Guardrails in `guardrails.c` and `agent_policy.c` are advisory text-matching, not enforcement
- No namespace, seccomp, or chroot isolation exists anywhere in the codebase

## Goals

- Tool-spawned processes (bash commands, file operations) run in isolated Linux namespaces by default.
- Filesystem access is restricted to the session workspace directory unless explicitly widened.
- Network access can be disabled per-tool or per-session.
- Sandboxing degrades gracefully inside containers (Docker/Podman) where `unshare` is unavailable.
- Existing guardrails (`pre_tool_check`) remain as a first-pass filter; sandboxing is defense-in-depth.

## Approach

### 1. Sandbox Configuration

Add a `SandboxConfig` struct with three filesystem modes:
- **off**: No isolation (current behavior, for trusted operations)
- **workspace_only**: Mount namespace restricts writes to session CWD
- **allowlist**: Explicit list of writable paths (e.g., CWD + /tmp)

Network isolation is a boolean flag (uses `CLONE_NEWNET` when enabled).

### 2. Process Spawning

Replace direct `fork()/exec()` in `platform_process.c` with a sandboxed variant that:
1. Calls `unshare(2)` with configured namespace flags
2. Bind-mounts allowed paths into a restricted root
3. Redirects `HOME` to `.sandbox-home/` and `TMPDIR` to `.sandbox-tmp/` within the workspace
4. Drops to an unprivileged UID within the user namespace
5. Executes the child command

### 3. Container Detection

Before attempting namespace isolation, check for container markers:
- `/.dockerenv`, `/run/.containerenv`
- `CONTAINER`, `DOCKER`, `PODMAN` env vars
- `/proc/1/cgroup` containing `docker`, `containerd`, `podman`

When inside a container, fall back to a restricted `PATH` + `chdir()` + `umask()` approach and log the degradation.

### 4. Integration Points

| File | Change |
|------|--------|
| `src/platform_process.c` | Add `platform_exec_sandboxed()` using `unshare(2)` |
| `src/agent_tools.c` | Route tool execution through sandboxed spawner |
| `src/config.c` | Add `sandbox.mode`, `sandbox.network`, `sandbox.allow_paths` config keys |
| `src/headers/platform.h` | Declare sandbox config struct and sandboxed exec API |
| `src/guardrails.c` | Keep as-is; sandbox is a separate layer |

## Acceptance Criteria

- [ ] `aimee delegate execute "cat /etc/shadow"` fails with EPERM when sandbox mode is `workspace_only`
- [ ] `aimee delegate execute "ls"` succeeds and shows workspace contents
- [ ] `aimee delegate execute "curl http://example.com"` fails when network isolation is enabled
- [ ] Running inside Docker with sandbox enabled logs a degradation warning and still restricts CWD
- [ ] `aimee config set sandbox.mode off` disables sandboxing entirely (opt-out)
- [ ] Existing guardrail tests still pass unchanged

## Owner and Effort

- **Owner:** aimee maintainer
- **Effort:** L (namespace isolation, mount plumbing, container detection, testing)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Default sandbox mode is `off` initially. Enable via `aimee config set sandbox.mode workspace_only`. Once validated, flip default to `workspace_only`.
- **Rollback:** `aimee config set sandbox.mode off` reverts to current behavior.
- **Blast radius:** Only affects tool execution in delegate sessions. Server, memory, indexing unaffected.

## Test Plan

- [ ] Unit tests: `platform_exec_sandboxed()` with each filesystem mode
- [ ] Integration tests: delegate session attempting out-of-workspace writes
- [ ] Failure injection: `unshare` unavailable (test container fallback path)
- [ ] Manual verification: run delegate in Docker container, confirm graceful degradation

## Operational Impact

- **Metrics:** `sandbox.mode` counter per session, `sandbox.fallback` counter for container degradation
- **Logging:** INFO on sandbox activation, WARN on fallback, ERROR on setup failure
- **Alerts:** None initially
- **Disk/CPU/Memory:** Minimal — namespace creation is a single syscall; mount setup adds ~5ms per tool invocation

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Namespace sandbox | P2 | L | High — defense-in-depth for all tool execution |

## Trade-offs

- **seccomp-bpf** was considered but adds significant complexity for marginal gain over namespace isolation. Can be layered on later.
- **Bubblewrap (bwrap)** would simplify implementation but adds an external dependency. Using raw `unshare(2)` keeps aimee dependency-free.
- **Sandbox-by-default** would be safest but risks breaking existing workflows. Opt-in first, then flip default after validation.
