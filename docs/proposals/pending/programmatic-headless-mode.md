# Proposal: Programmatic Headless Mode (`aimee run`)

## Problem

Aimee's `chat` command is interactive — it expects a human at the terminal. There is no way to invoke aimee programmatically for CI pipelines, scripts, cron jobs, or automated workflows. The `delegate` command handles remote execution but requires the server to be running and is designed for sub-tasks, not standalone programmatic use.

Mistral-vibe implements a `run_programmatic()` API with `--prompt`, `--max-turns`, `--max-price`, `--output-format` (text/json/streaming-json), and `--previous-messages`. This enables headless operation for CI, scripting, and integration into larger automation.

## Goals

- `aimee run --prompt "..." --output json` executes a task non-interactively and returns structured output.
- Configurable safety limits: max turns, max cost, max duration.
- Output in text (human-readable), JSON (structured), or NDJSON stream (real-time).
- Can be invoked from scripts, CI pipelines, and cron without a server.
- Works as both a CLI command and a webchat API endpoint (`POST /api/run`).

## Approach

### CLI: `aimee run`

```
aimee run --prompt "Review src/auth.c for security issues" \
          --max-turns 10 \
          --max-cost 0.50 \
          --timeout 300 \
          --output json \
          --model codex \
          --tools bash,read_file,grep
```

Internally: create an agent loop with the specified constraints, run it to completion, and emit structured output.

### Output Formats

**text**: Just the final assistant response (default).
```
Found 2 issues in src/auth.c: missing input validation on line 42, SQL injection risk on line 87.
```

**json**: Full structured result.
```json
{
  "status": "completed",
  "response": "Found 2 issues...",
  "turns": 4,
  "tokens": {"prompt": 3200, "completion": 450},
  "tool_calls": [
    {"tool": "read_file", "args": {"path": "src/auth.c"}, "status": "ok"},
    {"tool": "grep", "args": {"pattern": "sql.*query"}, "status": "ok"}
  ],
  "duration_ms": 12340
}
```

**ndjson**: Newline-delimited JSON stream (each event on a separate line, real-time).

### Webchat API

`POST /api/run` with JSON body mirroring the CLI flags. Returns the same structured output. SSE streaming for real-time results.

### Safety

- Default max turns: 20 (prevents runaway loops)
- Default max cost: $1.00 (prevents bill shock)
- Default timeout: 600s
- All defaults overridable via flags
- Non-zero exit code on failure

### Changes

| File | Change |
|------|--------|
| `src/cmd_core.c` | Add `run` subcommand with flag parsing |
| `src/agent.c` | Factor out headless agent loop (no interactive I/O) |
| `src/headers/agent.h` | Add `agent_run_headless()` API |
| `src/webchat.c` | Add `POST /api/run` endpoint |
| `src/render.c` | Add JSON and NDJSON output formatters |

## Acceptance Criteria

- [ ] `aimee run --prompt "echo hello" --output text` returns the response as plain text
- [ ] `aimee run --prompt "..." --output json` returns structured JSON with status, response, tokens, tool_calls
- [ ] `aimee run --prompt "..." --output ndjson` streams events in real-time
- [ ] `--max-turns 5` stops execution at 5 turns with status `"turn_limit"`
- [ ] `--max-cost 0.10` stops execution when cost exceeded with status `"cost_limit"`
- [ ] `--timeout 30` stops after 30 seconds with status `"timeout"`
- [ ] Exit code 0 on success, 1 on failure, 2 on limit reached
- [ ] `POST /api/run` in webchat produces identical output
- [ ] Pipe-friendly: `aimee run --prompt "..." | jq .response`

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2-3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** New command and API endpoint. No changes to existing commands.
- **Rollback:** Remove command. No persistent state changes.
- **Blast radius:** None — new functionality only.

## Test Plan

- [ ] Unit tests: flag parsing, output formatting, limit enforcement
- [ ] Integration tests: run a multi-turn task, verify JSON output structure
- [ ] Integration tests: verify each limit type (turns, cost, timeout) triggers correctly
- [ ] Failure injection: model API down — verify error in structured output
- [ ] Manual verification: use in a shell script, pipe to jq

## Operational Impact

- **Metrics:** `headless_run_count`, `headless_run_duration`, `headless_run_limit_hit{type=...}`
- **Logging:** INFO on run start/complete, WARN on limit hit
- **Alerts:** None
- **Disk/CPU/Memory:** Same as a normal agent session. No additional overhead.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Programmatic Headless Mode | P2 | M | High — enables CI integration, scripting, and automation |

## Trade-offs

**Alternative: Use `aimee delegate` for programmatic use.** Requires the server. `aimee run` is standalone and doesn't need the server running.

**Alternative: Just wrap `aimee chat` with expect/PTY scripting.** Fragile, no structured output, no safety limits. Not suitable for production automation.

**Known limitation:** Headless mode can't handle interactive tool approvals. Uses auto-approve with guardrails as the safety boundary (same as autonomous mode proposal).
