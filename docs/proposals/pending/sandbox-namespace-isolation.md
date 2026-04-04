# Proposal: Linux Namespace Sandbox for Tool Execution

## Problem

When aimee delegates tasks to agents or executes bash commands on their behalf, those commands run with the full privileges of the aimee process. An agent-generated `rm -rf /`, `curl | sh`, or accidental `DROP TABLE` runs without any OS-level isolation. Current guardrails (`guardrails.c`) operate at the command-string level — they can reject known-bad patterns but cannot prevent novel destructive commands or contain blast radius.

The `soongenwong/claudecode` repo at `rust/crates/runtime/src/sandbox.rs` implements a Linux namespace sandbox that:
- Uses `unshare` to isolate bash execution with user/mount/IPC/PID/UTS/network namespaces
- Supports filesystem isolation modes: off, workspace-only, explicit allow-list
- Detects container environments (Docker, Podman, Kubernetes) and adjusts behavior
- Falls back gracefully when namespace creation isn't available (nested containers, unprivileged)

This applies equally to CLI and webchat — any session that executes tools.

## Goals

- Agent-executed bash commands run in a Linux namespace sandbox by default, limiting filesystem access and network reach.
- Sandbox mode is configurable: off (current behavior), workspace-only (mount namespace restricts to CWD tree), or allow-list (explicit paths).
- Container environments are auto-detected and sandbox adapts (e.g., skip mount namespace inside Docker).
- Graceful fallback — if sandbox creation fails, commands still execute with a warning rather than hard-failing.
- Applies to all execution surfaces: CLI delegates, webchat sessions, MCP tool calls.

## Approach

### Sandbox Architecture

```
aimee agent_tools.c
  └── sandbox_exec(command, sandbox_config)
        ├── detect_container_env()  → Docker? Podman? K8s? bare metal?
        ├── build_unshare_args()    → --user --mount --ipc --pid --uts [--net]
        ├── setup_mount_namespace() → bind-mount workspace, /usr, /lib, /tmp
        └── exec_in_sandbox()       → unshare ... -- sh -c "command"
```

### Filesystem Isolation Modes

| Mode | Behavior |
|------|----------|
| `off` | No sandbox (current behavior) |
| `workspace` | Mount namespace: only CWD tree, /usr, /lib, /tmp are visible |
| `allowlist` | Mount namespace: only explicitly listed paths + /usr, /lib, /tmp |

### Container Detection

Check in order:
1. `/.dockerenv` exists → Docker
2. `/proc/1/cgroup` contains `docker` or `containerd` → Docker
3. `KUBERNETES_SERVICE_HOST` env → Kubernetes
4. `/run/.containerenv` exists → Podman
5. None of above → bare metal

When inside a container, skip user namespace (already namespaced) and mount namespace (bind mounts may fail). IPC/PID/UTS namespaces still work.

### Changes

| File | Change |
|------|--------|
| `src/sandbox.c` (new) | Namespace sandbox: container detection, unshare argument construction, mount namespace setup, sandboxed execution |
| `src/headers/sandbox.h` (new) | Public API: `sandbox_detect_env()`, `sandbox_exec()`, `sandbox_config_from_settings()` |
| `src/agent_tools.c` | Route bash execution through `sandbox_exec()` when sandbox is enabled |
| `src/config.c` | Add `sandbox_mode` config (off/workspace/allowlist), `sandbox_allowlist` paths |
| `src/guardrails.c` | Integrate sandbox as a defense-in-depth layer alongside pattern matching |

## Acceptance Criteria

- [ ] `sandbox_exec("rm -rf /", config)` with mode=workspace succeeds but only affects the workspace tree
- [ ] `sandbox_detect_env()` correctly identifies Docker, Podman, Kubernetes, and bare metal environments
- [ ] Inside Docker, sandbox skips user/mount namespaces and falls back gracefully
- [ ] `sandbox_exec()` with mode=off behaves identically to current `system()` execution
- [ ] Network namespace isolation blocks outbound connections when `network: false`
- [ ] CLI, webchat, and MCP tool execution all route through sandbox when enabled

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** M (2-3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Default mode is `off` — zero behavior change until explicitly configured. Can be set per-workspace or globally.
- **Rollback:** Set `sandbox.mode: "off"` in config.
- **Blast radius:** If sandbox is too restrictive, agent commands fail. Mitigation: fallback to unsandboxed execution with a warning.

## Test Plan

- [ ] Unit tests: container detection with mocked /proc and env vars, unshare argument construction per mode
- [ ] Integration tests: execute commands in workspace mode, verify files outside workspace are invisible
- [ ] Failure injection: unshare not available (permission denied), nested namespace (inside Docker)
- [ ] Manual verification: delegate a task with sandbox=workspace, confirm agent cannot read /etc/shadow but can edit workspace files

## Operational Impact

- **Metrics:** `sandbox_executions_total`, `sandbox_fallbacks_total`
- **Logging:** Sandbox mode and container detection at INFO on first use, fallbacks at WARN
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible overhead per command (~1ms for unshare setup)

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Core sandbox + container detection | P1 | M | High — security critical |
| Workspace isolation mode | P1 | S | High — most useful default |
| Network namespace isolation | P2 | S | Medium — prevents exfiltration |
| Allowlist mode | P3 | S | Low — niche use case |

## Trade-offs

- **Why not Docker/Podman for sandboxing?** Too heavy — starting a container per bash command adds 100ms+ overhead. `unshare` is kernel-native and near-zero overhead.
- **Why not seccomp/AppArmor?** Those filter syscalls, not filesystem visibility. Namespace isolation is a better fit for "agent can only see the workspace" semantics.
- **Why default to off?** Many users run aimee inside containers where nested namespaces may not work. Opt-in prevents breakage.

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/runtime/src/sandbox.rs`.
