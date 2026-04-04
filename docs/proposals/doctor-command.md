# Proposal: `aimee doctor` diagnostic command

## Problem

When aimee misbehaves -- hooks fail silently, the server won't start, MCP tools aren't registered, or a provider key is missing -- users have no structured way to diagnose the issue. They resort to manual inspection of config files, database state, and process lists. oh-my-openagent's `bunx oh-my-opencode doctor` demonstrates how a single diagnostic command can verify plugin registration, config validity, model availability, and environment health, cutting triage time dramatically.

aimee has no equivalent. The closest thing is `aimee init` which only checks database creation, and `aimee agent test` which only validates delegate connectivity.

## Goals

- A single command (`aimee doctor`) that checks all critical subsystems and reports pass/fail/warn for each.
- Machine-readable output via `--json` for scripted health checks and the webchat dashboard.
- Actionable remediation hints for each failure.

## Approach

Add a new `cmd_doctor` handler in `cmd_core.c` (or a new `cmd_doctor.c` if it grows large). The command runs a sequence of checks and prints a summary table.

### Checks

| Check | What it verifies |
|-------|-----------------|
| **Database** | DB file exists, is writable, schema version is current, FTS5 index is intact |
| **Server** | `aimee-server` process is running, socket is responsive |
| **Config** | `config.json` parses, required fields present, workspace paths exist |
| **Agents** | `agents.json` parses, at least one agent enabled, endpoints reachable (HEAD request) |
| **Hooks** | Hook scripts registered for detected AI tools (Claude Code, Gemini, Codex), hook scripts are executable |
| **MCP** | `.mcp.json` exists in workspace roots, `aimee mcp-serve` is listed |
| **Secrets** | Required API keys present in secret store (not values, just existence) |
| **Index** | At least one project indexed, index not stale (>24h since last update) |
| **Memory** | Memory table not empty, FTS5 consistency, no orphaned L0 entries older than 7 days |

### Output

```
$ aimee doctor
aimee doctor v0.2.0

  Database ............ OK   (schema v28, 1.2MB)
  Server .............. OK   (pid 4821, uptime 2h14m)
  Config .............. OK   (3 workspaces)
  Agents .............. WARN (claude-local: unreachable)
  Hooks ............... OK   (claude-code registered)
  MCP ................. OK   (2 workspace roots)
  Secrets ............. OK   (3 keys stored)
  Index ............... WARN (stale: last indexed 36h ago)
  Memory .............. OK   (L2: 142 facts, L3: 28 episodes)

2 warnings, 0 errors
Run with --fix to auto-repair detected issues.
```

`--fix` flag attempts automatic repairs: re-register hooks, rebuild FTS5 index, prune orphaned L0 memories, re-index stale projects.

### Webchat parity

Add a `/api/doctor` endpoint to `webchat.c` that returns the same check results as JSON. The webchat dashboard gets a new "Health" card that calls this endpoint and renders the results with colored badges (green/yellow/red).

### Changes

| File | Change |
|------|--------|
| `src/cmd_core.c` | Add `cmd_doctor` handler with check functions |
| `src/webchat.c` | Add `/api/doctor` endpoint returning JSON health results |
| `src/dashboard.c` | Add "Health" card to dashboard HTML |
| `src/headers/commands.h` | Declare `cmd_doctor` |
| `src/cmd_table.c` | Register `doctor` command |
| `src/cli_main.c` | Add `doctor` to usage text |

## Acceptance Criteria

- [ ] `aimee doctor` runs all checks and exits 0 on all-pass, 1 on warnings, 2 on errors
- [ ] `aimee --json doctor` returns structured JSON with per-check status, message, and remediation
- [ ] `--fix` flag auto-repairs at least: stale index, orphaned L0 memories, missing hook registration
- [ ] Webchat `/api/doctor` returns equivalent JSON
- [ ] Dashboard renders health card with auto-refresh

## Owner and Effort

- **Owner:** aimee contributor
- **Effort:** M (2-3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** New command, no migration needed. Dashboard card appears automatically.
- **Rollback:** Revert commit. No state changes unless `--fix` was run.
- **Blast radius:** None -- purely additive, read-only by default.

## Test Plan

- [ ] Unit tests: each check function in isolation (mock DB, mock socket, mock filesystem)
- [ ] Integration test: `aimee doctor` on a healthy install returns exit 0
- [ ] Integration test: deliberately break config, verify doctor catches it
- [ ] Manual verification: run `--fix` on stale index, confirm re-index triggers

## Operational Impact

- **Metrics:** None (doctor is on-demand).
- **Logging:** Each check logs at DEBUG level.
- **Disk/CPU/Memory:** Negligible. Checks are fast reads.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| `aimee doctor` | P2 | M | High -- drastically reduces triage time |

## Trade-offs

- **Alternative: per-subsystem status commands.** Rejected because the value is in running everything at once. Individual checks are already partially available (`agent test`, `init`).
- **Alternative: continuous health daemon.** Overkill. On-demand is sufficient; the dashboard card provides monitoring if needed.
