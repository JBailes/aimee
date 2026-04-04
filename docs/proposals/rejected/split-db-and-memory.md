# Proposal: Split db.c, memory.c, memory_promote.c, and memory_context.c

## Problem

db.c (1255 lines) conflates four distinct responsibilities: prepared statement caching, the migration system (33 migrations), backup/recovery, and the public API. memory.c (1137 lines), memory_promote.c (1057 lines), and memory_context.c (1017 lines) each slightly exceed the 1000-line target. memory_context.c contains a 300+ line function mixing cache lookup, DB queries, scoring, and formatting.

## Goals

- All resulting files under 1000 lines.
- Clear single responsibility per file.
- No functional changes.

## Approach

### db.c (1255 lines) splits into 3 files:

**db.c (~350 lines):** Core public API: `db_open`, `db_close`, `db_open_fast`, `db_prepare`, pragmas, `db_quick_check`. The statement cache stays here since it is tightly coupled to `db_prepare`.

**db_migrate.c (~550 lines):** Migration table (all 33 migrations), `db_migrate` runner, schema_migrations table management, version detection.

**db_backup.c (~300 lines):** `backup_before_migrate`, `db_recover`, manual backup command support, backup pruning.

### memory.c (1137 lines) trims to ~900 lines:

Extract `memory_search_windows_by_clusters` and related window-search functions into a new **memory_search.c (~240 lines)**. These are complex query builders that are only called from memory_context.c and memory.c search paths.

### memory_promote.c (1057 lines) trims to ~950 lines:

The promotion and demotion loops are nearly identical (both iterate kinds, load lifecycle rules, bind parameters). Extract a shared `apply_kind_lifecycle()` helper to eliminate ~100 lines of duplication.

### memory_context.c (1017 lines) splits into 2 files:

**memory_context.c (~550 lines):** Orchestrator: `memory_assemble_context`, cache handling, main assembly logic.

**memory_context_compact.c (~470 lines):** Window compaction: `memory_compact_windows`, cleanup logic, old window merging.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Reduce to core API + stmt cache (~350 lines) |
| `src/db_migrate.c` | New: migration system |
| `src/db_backup.c` | New: backup/recovery |
| `src/memory.c` | Extract window search (~900 lines) |
| `src/memory_search.c` | New: window search functions |
| `src/memory_promote.c` | Extract shared helper (~950 lines) |
| `src/memory_context.c` | Reduce to orchestrator (~550 lines) |
| `src/memory_context_compact.c` | New: compaction logic |
| `src/headers/db.h` | Update declarations, add db_migrate/db_backup headers or keep in db.h |
| `src/Makefile` | Add new .o files to appropriate layers |

## Acceptance Criteria

- [ ] db.c is under 400 lines
- [ ] No resulting file exceeds 1000 lines
- [ ] `make` builds clean with -Werror
- [ ] `make lint` passes
- [ ] All unit tests pass (especially test_memory_advanced, test_memory_health)
- [ ] Integration tests pass
- [ ] Database migration still works correctly (test by deleting DB and re-running)

## Owner and Effort

- **Owner:** TBD
- **Effort:** M (mechanical split, but migration table is large and must stay contiguous)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change, no behavior change.
- **Rollback:** git revert.
- **Blast radius:** Database and memory subsystems. All commands that use the DB are affected.

## Test Plan

- [ ] Unit tests: all memory tests pass unchanged
- [ ] Unit tests: db backup/recovery test
- [ ] Integration tests pass unchanged
- [ ] Manual: delete DB, run `aimee init`, verify migrations run correctly
- [ ] Manual: `aimee memory search`, `aimee memory list` work

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** None. Pure structural change.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Split db + memory files | P2 | M | Maintainability, review clarity |

## Trade-offs

**Why keep stmt cache in db.c instead of its own file?** The cache is only 80 lines and is called exclusively from `db_prepare`. Splitting it would create a trivial file with tight coupling to db.c.

**Why not split memory.c further?** At ~900 lines after extracting window search, it is within the target. The remaining functions (CRUD, search, list) are cohesive and belong together.

**Why split memory_context.c compaction into its own file?** Compaction operates on windows and is logically distinct from context assembly. It runs at different times (maintenance vs. query time) and touches different tables.
