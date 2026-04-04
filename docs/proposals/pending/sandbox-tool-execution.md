# Proposal: Sandboxed Tool Execution with Linux Namespace Isolation

## Problem

The pending set has two proposals describing the same underlying feature: a Linux namespace sandbox around tool execution. One emphasizes the user-facing security goal, the other the implementation details. They should be one proposal.

Today tool-executed commands still run with the effective privileges of the aimee process. Guardrails catch known-bad strings, but they are not a security boundary. A misbehaving agent can still attempt filesystem corruption, data exfiltration, or shell-based escalation in ways that pattern matching will miss.

## Goals

- Run shell-backed tool execution inside an OS-level sandbox.
- Restrict filesystem visibility to the workspace or an explicit allowlist.
- Allow network isolation where supported.
- Degrade gracefully in nested/containerized environments.
- Keep existing guardrails as a first-pass policy layer, with sandboxing as enforcement.

## Approach

Implement one sandbox subsystem around process spawning, based on Linux namespaces and explicit runtime configuration.

### Sandbox Modes

| Mode | Behavior |
|------|----------|
| `off` | Current behavior |
| `workspace_only` | Restrict visible/writable filesystem to the session workspace and runtime essentials |
| `allowlist` | Restrict access to configured paths plus runtime essentials |

Additional controls:

- optional network namespace isolation
- redirected `HOME` and `TMPDIR` into sandbox-local locations
- container detection with graceful fallback when `unshare` or bind mounts are unavailable

### Integration

| File | Change |
|------|--------|
| `src/sandbox.c` | Namespace setup, environment detection, restricted exec path |
| `src/headers/sandbox.h` | Config structs and sandbox execution API |
| `src/platform_process.c` | Add sandbox-aware process launcher |
| `src/agent_tools.c` | Route tool execution through sandbox-aware spawner |
| `src/config.c` | Parse `sandbox.mode`, `sandbox.network`, `sandbox.allow_paths` |
| `src/guardrails.c` | Keep current checks, but treat sandboxing as defense-in-depth |

## Acceptance Criteria

- [ ] Workspace-only mode prevents out-of-workspace reads and writes while preserving normal workspace operations.
- [ ] Network-isolated mode blocks outbound network access when supported.
- [ ] Container detection falls back safely with a warning rather than crashing execution.
- [ ] `sandbox.mode=off` preserves current behavior.
- [ ] CLI, delegates, and MCP-triggered tool execution all use the same sandbox path.

## Owner and Effort

- **Owner:** aimee maintainer
- **Effort:** L
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Start opt-in with `off` as the default, validate in real workflows, then consider flipping the default to `workspace_only`.
- **Rollback:** Set `sandbox.mode=off`.
- **Blast radius:** Overly restrictive sandbox settings can break legitimate tool execution; fallback and clear diagnostics are required.

## Test Plan

- [ ] Unit tests for environment detection and config translation.
- [ ] Integration tests for workspace-only and allowlist modes.
- [ ] Failure injection for unavailable `unshare`, nested containers, and mount failures.
- [ ] Manual verification that dangerous paths are blocked while workspace tasks still succeed.

## Operational Impact

- **Metrics:** `sandbox_executions_total`, `sandbox_fallbacks_total`
- **Logging:** INFO on sandbox activation, WARN on fallback/degradation
- **Alerts:** None
- **Disk/CPU/Memory:** Small per-command overhead

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Core namespace sandbox | P1 | L | High |
| Workspace-only mode | P1 | S | High |
| Container fallback | P1 | S | High |
| Network isolation | P2 | S | Medium |
| Allowlist mode | P2 | S | Medium |

## Trade-offs

- **Why merge both sandbox proposals?** The implementation and product framing are inseparable here; splitting them creates duplicate rollout plans and contradictory effort estimates.
- **Why not rely on guardrails alone?** Guardrails are policy hints. Sandbox isolation is the enforcement layer.
- **Why opt-in first?** Nested container environments can be tricky; rollout needs real-world validation before changing defaults.
