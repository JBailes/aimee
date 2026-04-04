# Proposal: Session history CLI and webchat browser

## Problem

aimee's server tracks sessions (`session.create`, `session.list`, `session.get`, `session.close`) and stores delegation logs, traces, and memory per session. But there's no CLI command to browse, search, or analyze past sessions. Users can't answer "what did I work on yesterday?" or "find the session where we fixed the auth bug" without manually querying the database.

oh-my-openagent provides session history tools: list, read, search, and analyze. This is valuable for continuity across conversations, auditing agent work, and understanding patterns in how agents are used.

## Goals

- CLI commands to list, search, inspect, and summarize past sessions.
- Webchat session browser that lets users navigate session history.
- Session search integrated with aimee's FTS5 memory search.

## Approach

### New CLI commands

```
aimee session list [--since DATE] [--limit N]
aimee session show <session-id>
aimee session search <query>
aimee session stats [--since DATE]
```

**`session list`**: Shows recent sessions with start time, duration, delegation count, and a one-line summary (derived from the first L0 memory or the session's working memory).

```
$ aimee session list --limit 5
ID          STARTED              DURATION  DELEGATIONS  SUMMARY
a3f8c21e    2026-04-04 10:15     42m       3            Refactored auth middleware
b9d1e47f    2026-04-03 14:30     1h18m     7            Fixed webchat SSL cert renewal
c7e2a89b    2026-04-03 09:00     25m       1            Updated infrastructure docs
...
```

**`session show`**: Displays full session detail -- all delegations, traces, memory writes, tool calls, and working memory state.

**`session search`**: FTS5 search across session summaries, delegation prompts, and working memory. Returns matching sessions ranked by relevance.

**`session stats`**: Aggregate metrics -- sessions per day, average duration, delegations per session, most-used roles, token usage trends.

### Implementation

The data is already in the database (sessions, agent_log, traces, working_memory tables). This proposal adds CLI commands that query and format it.

### Webchat parity

Add a `/sessions` page to the webchat UI (alongside the existing chat and dashboard). This page shows a searchable, paginated list of past sessions. Clicking a session shows its full timeline: delegations, tool calls, memory writes, in chronological order.

API endpoints:
- `GET /api/sessions?since=DATE&limit=N` -- session list
- `GET /api/sessions/:id` -- session detail
- `GET /api/sessions/search?q=QUERY` -- FTS5 search
- `GET /api/sessions/stats` -- aggregate stats

### Changes

| File | Change |
|------|--------|
| `src/cmd_core.c` or `src/cmd_session.c` (new) | `cmd_session` handler with list/show/search/stats subcommands |
| `src/server_state.c` | Add session history query handlers |
| `src/webchat.c` | Add `/api/sessions/*` endpoints and sessions page HTML |
| `src/cmd_table.c` | Register `session` command |
| `src/cli_main.c` | Add `session` to usage text |
| `src/db.c` | Add session summary view/index if needed for performance |

## Acceptance Criteria

- [ ] `aimee session list` shows recent sessions with summary
- [ ] `aimee session show <id>` shows full session timeline
- [ ] `aimee session search <query>` returns relevant sessions via FTS5
- [ ] `aimee session stats` shows usage aggregates
- [ ] `aimee --json session list` returns structured JSON
- [ ] Webchat `/api/sessions` endpoints return equivalent data
- [ ] Webchat sessions page renders session list and detail views

## Owner and Effort

- **Owner:** aimee contributor
- **Effort:** M (2-3 days)
- **Dependencies:** None -- data is already collected

## Rollout and Rollback

- **Rollout:** New commands and API endpoints. No migration needed (queries existing tables).
- **Rollback:** Revert commit. No data changes.
- **Blast radius:** None -- read-only commands.

## Test Plan

- [ ] Unit tests: session list/show/search query builders
- [ ] Integration test: create session, close it, verify it appears in `session list`
- [ ] Integration test: search for a term used in a delegation prompt, verify session is found
- [ ] Manual verification: browse sessions in webchat UI

## Operational Impact

- **Metrics:** None (on-demand queries).
- **Logging:** Queries logged at DEBUG.
- **Disk/CPU/Memory:** Session search may be slow on very large databases; consider adding a summary index if needed.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Session history | P2 | M | High -- enables continuity and audit |

## Trade-offs

- **Alternative: export to external tool.** Rejected because the value is in quick inline access, not generating reports.
- **Limitation:** Session summaries depend on L0 memory or working memory being populated. Sessions without any memory writes will have sparse summaries.
