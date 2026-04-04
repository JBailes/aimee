# Proposal: Schema Version Fast-Path for db_open

## Problem

`db_open()` runs the full migration loop (`migrate()` in `src/db.c:679-741`) on every open. This iterates over 32 migration entries, running a `SELECT 1 FROM schema_migrations WHERE version = ?` for each to check if it's been applied. On an already-migrated database, this is 32 prepared statements + step + finalize cycles for zero work done.

## Goals

- Skip the per-migration check loop when the database is already at the latest schema version.
- `db_open()` migration overhead reduced from ~32 queries to 1 on a fully-migrated database.

## Approach

Use SQLite's `PRAGMA user_version` to store the current schema version. At the end of `migrate()`, set `user_version` to `MIGRATION_COUNT`. At the start of `migrate()`, read `user_version` — if it equals `MIGRATION_COUNT`, return immediately.

`PRAGMA user_version` is a single integer stored in the database header (page 1, offset 60). Reading it requires no table access and is effectively free.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | At the start of `migrate()`: read `PRAGMA user_version`; if it equals `MIGRATION_COUNT`, return 0 immediately. After successfully running all migrations: set `PRAGMA user_version = MIGRATION_COUNT` |

## Acceptance Criteria

- [ ] `migrate()` returns in <1ms on a fully-migrated database (single PRAGMA read)
- [ ] New databases still run all migrations and set `user_version`
- [ ] Databases migrated before this change get the `user_version` set on next open (one final full loop, then fast-path on subsequent opens)
- [ ] All existing tests pass

## Owner and Effort

- **Owner:** JBailes
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change. Existing databases will run one more full migration loop to set `user_version`, then benefit from the fast-path.
- **Rollback:** `git revert`. The `user_version` pragma is harmless if the check code is removed.
- **Blast radius:** Database open path only.

## Test Plan

- [ ] Unit tests: `db_open()` on a fresh database runs all migrations and sets `user_version`
- [ ] Unit tests: `db_open()` on an already-migrated database skips migration loop
- [ ] Unit tests: adding a new migration (incrementing `MIGRATION_COUNT`) causes the loop to run again
- [ ] Integration tests: all database operations work correctly after fast-path open

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** Saves ~32 SQLite queries per `db_open()` on migrated databases.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Schema version fast-path | P2 | S | Saves 10-50ms per db_open |

## Trade-offs

If someone manually modifies the database schema outside of aimee (e.g., drops a table), the `user_version` will still indicate "fully migrated" and the migration won't re-run. This is acceptable because manual schema modification is unsupported. The `CREATE TABLE IF NOT EXISTS` statements in migrations are idempotent anyway — the real cost is the 32-query loop, which this eliminates.
