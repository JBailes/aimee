# Proposal: Migration Rollback and Corruption Recovery

## Problem

aimee's SQLite database evolves through inline migrations in `db.c` (currently
defined via `MIGRATION_COUNT` macro at `db.c:614`). Transaction wrapping exists
(`db.c:646` uses `BEGIN IMMEDIATE`/`ROLLBACK`/`COMMIT` per migration), but there
is no pre-migration backup, no corruption detection, and no recovery playbook.
Since all state (memories, rules, tasks, agent log, session data) lives in a
single `.db` file, data loss from a failed migration or disk error is
catastrophic.

## Goals

- Database is backed up before any migration runs.
- Corruption is detected early and recovery is automated where safe.
- Manual recovery is possible without data loss expertise.
- No silent destructive recovery — operator confirmation required for data loss.

## Approach

### 1. Pre-Migration Backup

Before running any pending migration, back up the database using the SQLite
online backup API (`sqlite3_backup_init()`/`sqlite3_backup_step()`). This is
atomic — a partial backup cannot be left behind.

Keep the last 3 backups: `aimee.db.bak.N`, `aimee.db.bak.N-1`, `aimee.db.bak.N-2`.
Delete older backups after a successful migration.

Rationale for N=3: If corruption predates the latest migration (e.g., a bug in
migration 27 corrupts data, then migration 28 backs up the corrupt state), a
single backup is useless. Three backups provide a reasonable recovery window.

```c
static int backup_before_migrate(sqlite3 *db, const char *db_path, int current_version)
{
   char bak[MAX_PATH_LEN];
   snprintf(bak, sizeof(bak), "%s.bak.%d", db_path, current_version);

   sqlite3 *dst;
   if (sqlite3_open(bak, &dst) != SQLITE_OK) return -1;

   sqlite3_backup *b = sqlite3_backup_init(dst, "main", db, "main");
   if (!b) { sqlite3_close(dst); return -1; }

   sqlite3_backup_step(b, -1);  /* copy entire db in one step */
   sqlite3_backup_finish(b);
   sqlite3_close(dst);

   /* Prune old backups: keep only last 3 */
   for (int v = current_version - 3; v >= 0; v--) {
      char old[MAX_PATH_LEN];
      snprintf(old, sizeof(old), "%s.bak.%d", db_path, v);
      unlink(old);  /* ignore errors for missing files */
   }
   return 0;
}
```

Location: `db.c:migrate()` — call before the migration loop when pending
migrations are detected.

**Decision: If backup fails, skip the migration.** Running a migration without
a safety net on a disk that can't write is worse than running on a stale
schema. The migration will succeed on the next invocation when disk space is
available. Log a clear warning:
```
aimee: WARNING: pre-migration backup failed (disk full?), skipping migration.
       Database remains at schema version N. Free disk space and restart.
```

### 2. Migration Transaction Safety

Already implemented: `db.c:646-674` wraps each migration in `BEGIN IMMEDIATE` /
`COMMIT` with `ROLLBACK` on failure. No change needed.

### 3. Corruption Detection

Run `PRAGMA quick_check` (not `integrity_check` — the full check is too slow for
startup) during `session-start` only. This runs once per session rather than on
every `db_open()` or `db_open_fast()` call, avoiding the latency impact on the
hot path.

Location: `cmd_hooks.c:cmd_session_start()` — after `db_open()`, before any
other operations.

Recovery sequence on failure:
1. `PRAGMA quick_check` returns errors
2. Log error to stderr: `aimee: database corruption detected`
3. Close the corrupted database
4. Search for `aimee.db.bak.*` — find most recent with valid `quick_check`
5. Copy backup over corrupted file (using SQLite backup API for atomicity)
6. Re-open and re-migrate to bring schema up to date
7. Log: `aimee: recovered from backup version N`

If no valid backup exists: **do not create a fresh database silently**. Print an
error and instructions:
```
aimee: database corrupted and no valid backup found.
To start fresh (all memories, rules, and tasks will be lost):
  aimee db recover --force
```

### 4. Manual Recovery Command

`aimee db recover` — attempts recovery from backup. With `--force`, creates a
fresh database if no backup exists.

`aimee db backup` — creates a manual backup (useful before risky operations).

`aimee db check` — runs full `PRAGMA integrity_check` and reports results.

Location: New subcommand group in `cmd_core.c`.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Pre-migration backup with SQLite backup API, backup pruning |
| `src/cmd_hooks.c` | `PRAGMA quick_check` during session-start |
| `src/cmd_core.c` | `aimee db recover`, `aimee db backup`, `aimee db check` subcommands |
| `src/cmd_table.c` | Register `db` command |

## Acceptance Criteria

- [ ] `aimee.db.bak.N` is created before any pending migration runs
- [ ] At most 3 backup files exist at any time
- [ ] `PRAGMA quick_check` runs once during `session-start` (not on every command)
- [ ] Corrupted database triggers automatic recovery from most recent valid backup
- [ ] If no valid backup exists, recovery stops with an error and instructions
- [ ] `aimee db recover --force` creates a fresh database with operator confirmation
- [ ] `aimee db backup` creates a manual backup
- [ ] `aimee db check` runs full integrity check and reports results

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Ships with next binary build. Backup creation happens automatically
  on first migration after upgrade. Corruption detection activates on next
  `session-start`.
- **Rollback:** Revert commit. Backup files on disk are harmless and can be
  deleted manually.
- **Blast radius:** If backup creation fails (disk full), the migration is
  skipped — aimee continues with the current schema version and logs a warning.
  This prevents running a migration without a safety net. Corruption detection
  is read-only and cannot cause damage.

## Test Plan

- [ ] Unit test: `backup_before_migrate()` creates a valid backup file
- [ ] Unit test: backup pruning keeps only 3 most recent
- [ ] Integration test: introduce corruption (truncate db file), verify
      `session-start` detects it and recovers from backup
- [ ] Failure injection: interrupt migration (fork + kill child mid-migrate),
      verify `ROLLBACK` leaves database in pre-migration state
- [ ] Failure injection: fill disk during backup, verify migration is SKIPPED
      with warning (not proceeded with)
- [ ] Failure injection: corrupt db with no backup, verify `aimee db recover`
      prints error (not silent fresh create)
- [ ] Integration test: `aimee db check` on healthy database returns "ok"

## Operational Impact

- **Metrics:** None (no new counters).
- **Logging:** Backup creation logged to stderr. Corruption detection logged.
  Recovery actions logged.
- **Alerts:** None (single-operator system).
- **Disk/CPU/Memory:** 3 backup files ≈ 3x database size. `quick_check` adds
  ~5-20ms to session-start depending on database size.
- **Security:** Backup files contain the same sensitive data as the primary DB
  (API keys, session tokens, memories). `backup_before_migrate()` must set
  `chmod(bak, 0600)` on each backup file. The existing `db_open()` already
  does `chmod(path, 0600)` for the primary DB (`db.c:746`).

## Priority

P1 — data loss prevention for a system that stores persistent knowledge.

## Trade-offs

**Why `quick_check` instead of `integrity_check`?** Full integrity check scans
every page and can take 50-200ms on a large database. `quick_check` verifies
B-tree structure without scanning all data — sufficient to catch file-level
corruption (truncation, bit rot) while staying under 20ms.

**Why not per-table backups?** SQLite's backup API copies the entire database
atomically. Per-table export/import is more complex, slower, and doesn't preserve
indexes or triggers.

**Why 3 backups instead of 1?** A single backup only helps if corruption happens
*after* the backup. If corruption predates the latest migration, the backup
contains corrupt data. Three versions provide a multi-day recovery window.
