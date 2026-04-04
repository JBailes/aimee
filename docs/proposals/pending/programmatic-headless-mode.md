# Proposal: Programmatic Headless Mode and Structured Output

## Problem

Programmatic execution and structured output are currently split across separate proposals:

- a headless `aimee run` mode
- NDJSON output for machine consumers

These belong together. A non-interactive execution surface is only truly useful if it has stable machine-readable output modes.

## Goals

- Run aimee non-interactively from scripts, CI, cron, or APIs.
- Support text, JSON, and NDJSON output modes.
- Enforce explicit safety limits such as max turns, max cost, and timeout.
- Reuse one event schema across CLI and API surfaces.

## Approach

Add `aimee run` as a headless agent entry point with structured output modes.

### CLI

```bash
aimee run --prompt "Review src/auth.c for security issues" \
          --max-turns 10 \
          --max-cost 0.50 \
          --timeout 300 \
          --output json
```

### Output Modes

- `text`: final human-readable response
- `json`: one structured final result
- `ndjson`: one structured event per line for streaming consumers

The NDJSON event schema should align with the streaming event model used by interactive surfaces where possible.

### API

Expose a matching `POST /api/run` endpoint using the same limits and output schema.

### Changes

| File | Change |
|------|--------|
| `src/cmd_core.c` | Add `run` subcommand |
| `src/agent.c` | Factor out headless execution path |
| `src/headers/agent.h` | Headless execution API |
| `src/render.c` | JSON and NDJSON output formatters |
| `src/webchat.c` | Add `POST /api/run` endpoint |

## Acceptance Criteria

- [ ] `aimee run --prompt "..." --output text` returns plain text.
- [ ] `aimee run --prompt "..." --output json` returns one structured JSON object.
- [ ] `aimee run --prompt "..." --output ndjson` streams parseable NDJSON events.
- [ ] Turn, cost, and timeout limits stop execution with explicit status.
- [ ] API output matches CLI output semantics.
- [ ] Default human-readable behavior remains straightforward.

## Owner and Effort

- **Owner:** aimee
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Additive new command and API endpoint.
- **Rollback:** Remove the command and endpoint; no migration required.
- **Blast radius:** None outside the new headless path.

## Test Plan

- [ ] Unit tests: flag parsing, output formatting, limit enforcement
- [ ] Integration tests: text/json/ndjson outputs
- [ ] Integration tests: API and CLI parity
- [ ] Manual verification: shell-script and `jq` usage

## Operational Impact

- **Metrics:** `headless_run_count`, `headless_run_duration`, `headless_run_limit_hit`
- **Logging:** INFO on run start/finish, WARN on limit hits
- **Alerts:** None
- **Disk/CPU/Memory:** Same as a normal agent session

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Headless run command | P1 | M | High |
| JSON output | P1 | S | High |
| NDJSON event stream | P2 | S | Medium |

## Trade-offs

- **Why merge headless mode and NDJSON?** The output contract is part of the product surface for automation.
- **Why keep text mode too?** Headless does not mean machine-only; shell users still want a simple default.
- **Why not build only an API?** Local CLI automation is a first-class use case.
