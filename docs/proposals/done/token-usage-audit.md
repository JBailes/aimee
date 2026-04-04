# Proposal: Token Usage Auditing

## Problem

There is currently no granular visibility into which tools, projects, or agent
roles are the most expensive in terms of tokens. `agent_log` tracks tokens per
request, but it does not provide the cross-cut summaries needed to answer
questions like:

- which roles are driving most cost
- which projects are most expensive
- how much session-start context injection costs over time

## Goals

- Track token consumption across tools, projects, and sessions.
- Identify "high-blast-radius" operations that consume excessive tokens.
- Provide a dashboard or CLI summary of usage and estimated costs.

## Approach

Implement a lightweight auditing layer that builds on `agent_log` and adds
summary-friendly attribution where current logs are too coarse.

### Schema Changes

```sql
CREATE TABLE IF NOT EXISTS token_audit (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id TEXT NOT NULL,
  project_name TEXT,
  tool_name TEXT NOT NULL,
  role TEXT,
  prompt_tokens INTEGER DEFAULT 0,
  completion_tokens INTEGER DEFAULT 0,
  estimated_cost_usd REAL DEFAULT 0.0,
  created_at TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX idx_audit_session ON token_audit(session_id);
CREATE INDEX idx_audit_tool ON token_audit(tool_name);
```

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Add `token_audit` table only if `agent_log` aggregation proves insufficient |
| `src/agent.c` | Record role/project attribution for delegate token usage |
| `src/cmd_hooks.c` | Record session-start context assembly token estimates where available |
| `src/cmd_core.c` or new reporting command | Add a usage summary command |

## Acceptance Criteria

- [ ] `aimee util usage --last 24h` shows total tokens and estimated cost.
- [ ] Usage is broken down by tool (e.g., Grep vs. Read vs. Delegate).
- [ ] Minimal overhead on tool execution (< 5ms).

## Owner and Effort

- **Owner:** Agent
- **Effort:** S (2 days)
- **Dependencies:** None

## Test Plan

- [ ] Unit tests: Verify audit records are created correctly.
- [ ] Integration tests: Run a complex task and check aggregated usage stats.
- [ ] Manual verification: View usage report after a session.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Audit Table | P1 | S | Low |
| Agent Integration | P1 | S | High |
| Usage Report | P2 | S | Medium |
