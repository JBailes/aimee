# Proposal: SQLite & JSON Boilerplate Macros

## Problem
Mapping SQLite rows to structural types and building JSON objects via `cJSON` currently requires extensive, manual boilerplate. Each database query involves repetitive `sqlite3_column_text` and `sqlite3_bind_*` calls that clutter the logic layers.

## Goals
- Standardize data mapping using a robust set of C macros.
- Improve code density and readability in `db.c` and `memory.c`.

## Approach
Introduce `aimee_db.h` with macros for common mapping patterns:
- `DB_BIND_STR(stmt, idx, val)`: Handles null-checks and transient binding.
- `DB_FETCH_STR(m, field, stmt, col)`: Maps a column directly to a struct field with safety checks.
- `JSON_ADD_STR(obj, key, val)`: Wraps `cJSON_AddStringToObject` with null safety.

## Acceptance Criteria
- [ ] Macros implemented and adopted in `memory.c`, `agent_config.c`, and `tasks.c`.
- [ ] Subsystem line counts reduced by ~10%.
- [ ] No regression in database performance or JSON serialization integrity.
