# Agent Platform Features, Phase 2

## Problem

Phase 1 added tool registry, policy, trace, confidence, and metrics.
Several gaps remain: there is no local visibility into agent behavior,
agents have to guess project conventions (build/test commands), long
tasks cannot survive terminal restarts, execution results are not
machine-readable, and user preferences lack granularity.

## Features

### 10. Local Observability Dashboard

Serve a lightweight HTML dashboard from `aimee dashboard` on a local
port. Reads directly from aimee.db. Single-page app with:

- Live delegation log (recent runs with status, turns, latency, confidence)
- Plan graph (steps with status, dependencies)
- Memory promotions/demotions over time
- Token cost by agent/role
- Tool call frequency

Implementation: embed a static HTML/JS page in the C binary as a
string constant. `aimee dashboard [--port N]` starts an HTTP server
using a minimal embedded server (single-threaded, select-based). Serves
`/` (dashboard HTML), `/api/delegations` (JSON), `/api/plans` (JSON),
`/api/memory-stats` (JSON), `/api/metrics` (JSON). Default port 9200.

### 11. Repo Contract File (.aimee/project.yaml)

Project-specific execution contracts in `.aimee/project.yaml`:

```yaml
name: acktng
language: c
build: cd src && make
test: cd src && make unit-tests
lint: cd src && make lint
risky_paths:
  - src/db_*        # database layer, requires review
  - src/comm.c      # network layer, requires review
definition_of_done:
  - build succeeds
  - tests pass
  - lint clean
```

Loaded by `agent_build_exec_context()` and injected into the sub-agent
system prompt. The sub-agent knows exactly how to build, test, and lint
without guessing.

Also used by the eval harness to verify "definition of done" after
agent execution.

### 12. Stateful Long-Running Sessions

Durable jobs that survive crashes and terminal restarts:

```sql
CREATE TABLE agent_jobs (
   id INTEGER PRIMARY KEY,
   role TEXT NOT NULL,
   prompt TEXT NOT NULL,
   agent_name TEXT NOT NULL,
   status TEXT NOT NULL DEFAULT 'pending',
   cursor TEXT,           -- JSON: last completed turn/step
   heartbeat_at TEXT,     -- last activity timestamp
   lease_owner TEXT,      -- PID that owns this job
   result TEXT,           -- JSON result on completion
   created_at TEXT NOT NULL DEFAULT (datetime('now')),
   updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);
```

`aimee delegate --durable` creates a job record. The agentic loop
updates `cursor` after each turn. On crash, `aimee jobs resume <id>`
picks up from the last cursor (replays the conversation from trace,
then continues).

`aimee jobs list` shows all jobs. `aimee jobs status <id>` shows
progress. `aimee jobs cancel <id>` stops a running job.

Heartbeat: the loop writes `heartbeat_at` every turn. A job is
considered stale if heartbeat is older than 5 minutes (configurable).
`aimee jobs resume` only picks up stale jobs (prevents double-execution).

### 13. Automatic Environment Introspection

Before the first execution, detect and cache environment capabilities:

```sql
CREATE TABLE env_capabilities (
   key TEXT PRIMARY KEY,
   value TEXT NOT NULL,
   detected_at TEXT NOT NULL DEFAULT (datetime('now'))
);
```

Detection checks:
- Package managers: apt, brew, dnf, pacman (which/command -v)
- Toolchains: gcc, clang, dotnet, node, python3, go, rustc
- CI config: .github/workflows/, .gitlab-ci.yml, Jenkinsfile
- SSH connectivity: can reach deploy host
- Available disk space, memory

`agent_introspect_env()` runs once on first delegation (or on
`aimee env detect`). Results cached in `env_capabilities`. Injected
into the sub-agent system prompt via `agent_build_exec_context()`.

### 14. Machine-Readable Change Manifests

After each agent execution, emit a structured manifest:

```json
{
   "run_id": "aimee-task-12345",
   "agent": "codex",
   "role": "deploy",
   "timestamp": "2026-03-30T19:15:00Z",
   "files_touched": [
      {"path": "/etc/prometheus/prometheus.yml", "action": "read"},
      {"path": "/tmp/deploy.sh", "action": "write", "reason": "staging deploy script"}
   ],
   "commands_run": [
      {"command": "ssh deploy@192.168.1.101 ...", "exit_code": 0},
      {"command": "curl http://10.1.0.247:5000/health", "exit_code": 0}
   ],
   "checks": [
      {"name": "health-check", "outcome": "pass"}
   ],
   "confidence": 90,
   "turns": 2,
   "tool_calls": 3
}
```

Written to `~/.config/aimee/manifests/<run_id>.json`. Generated from
the execution trace table after each run. `aimee manifest list`,
`aimee manifest show <id>`.

### 15. Human Preference Controls (Hard/Soft Directives)

Separate user guidance into three tiers:

```sql
ALTER TABLE rules ADD COLUMN directive_type TEXT DEFAULT 'soft';
-- 'hard': must never violate (blocks execution)
-- 'soft': optimize when possible (warning only)
-- 'session': temporary override, expires at session end
ALTER TABLE rules ADD COLUMN expires_at TEXT;
```

Hard directives are injected into the sub-agent system prompt with
"MUST" language. Soft directives use "SHOULD" language. Session
directives auto-expire when the session ends.

CLI:
- `aimee + --hard "Never push to main"` (hard constraint)
- `aimee + --soft "Prefer concise output"` (soft preference)
- `aimee + --session "Skip tests for this run"` (session override)

During execution, before each tool call, check hard directives. If a
tool call would violate a hard directive (detected via keyword matching
on the command), block it and return an error to the LLM.

## Implementation Order

10. Local dashboard (standalone, reads from DB)
11. Repo contract (enhances context injection)
12. Long-running sessions (enhances agentic loop)
13. Environment introspection (enhances context injection)
14. Change manifests (post-processing of trace)
15. Preference controls (enhances rules + guardrails)

## Affected Files

| Feature | Files |
|---------|-------|
| 10 | `src/dashboard.c` (new), `src/headers/dashboard.h` (new), `src/main.c`, `Makefile` |
| 11 | `src/agent.c` (context injection), `src/headers/agent.h` |
| 12 | `src/db.c` (migration), `src/agent.c` (loop cursor), `src/main.c` (jobs CLI) |
| 13 | `src/agent.c` (introspection), `src/db.c` (migration) |
| 14 | `src/agent.c` (manifest generation), `src/main.c` (manifest CLI) |
| 15 | `src/db.c` (migration), `src/rules.c` (directive types), `src/agent.c` (hard checks) |
