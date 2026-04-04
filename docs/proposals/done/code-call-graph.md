# Proposal: Code Call Graph Indexing

## Problem

Aimee's current code indexer (`index.c`, `extractors.c`) identifies symbol definitions but lacks awareness of how symbols *interact* (which function calls which). This makes "blast radius" analysis surface-level (file dependencies only) and forces agents to use many Grep calls to trace execution flow, consuming significant tokens.

## Goals

- Map relationships between functions and classes (Call Graph).
- Improve `blast-radius` accuracy by tracing symbol usage across files.
- Enable semantic navigation (e.g., "show me all callers of X").

## Approach

Enhance code extractors to identify function/method calls and store them in a new `code_calls` table. Update the blast-radius tool to leverage this graph.

### Schema Changes

```sql
CREATE TABLE IF NOT EXISTS code_calls (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
  caller_id INTEGER NOT NULL REFERENCES terms(id) ON DELETE CASCADE,
  callee_name TEXT NOT NULL,
  line INTEGER NOT NULL
);
CREATE INDEX idx_cc_callee ON code_calls(callee_name);
```

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Add migration #38 for `code_calls` table and index. |
| `src/extractors.c` | Update extractors (C, Python, etc.) to capture function calls. |
| `src/index.c` | Update `index_scan_project` to populate `code_calls`. |
| `src/cmd_index.c` | Add `aimee index callers <symbol>` and update `blast-radius`. |

## Acceptance Criteria

- [ ] `aimee index callers my_function` returns a list of files and line numbers where `my_function` is invoked.
- [ ] `blast-radius` includes symbols that directly call the modified symbol.
- [ ] Indexing time for a 100-file project increases by < 20%.

## Owner and Effort

- **Owner:** Agent
- **Effort:** M (3-5 days)
- **Dependencies:** None

## Test Plan

- [ ] Unit tests: Verify call extraction for supported languages.
- [ ] Integration tests: Verify blast-radius outputs for known call chains.
- [ ] Manual verification: Trace a call stack using `aimee index callers`.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Schema + Extraction | P1 | M | High |
| Callers Tool | P2 | S | Medium |
| Enhanced Blast-Radius | P2 | S | High |
