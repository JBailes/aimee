# Proposal: SQLite Reliability Profile for Server Mode

## Problem

SQLite pragmas are set inconsistently across open paths:

- `db_open()` (`db.c:830-836`): Sets WAL, busy_timeout 5000ms, foreign_keys ON.
- `db_open_fast()` (`db.c:894-896`): Same pragmas, skips migrations.
- Server mode (`db.c:517-518`): Adds `cache_size = -8192` (8MB), `mmap_size = 67108864` (64MB).

Missing:
1. **No explicit `synchronous` pragma.** WAL defaults to `FULL`, but this is not
   documented or enforced. A future code change could inadvertently weaken durability.
2. **No startup self-check.** Corruption is only detected if a query fails.
3. **No operator-facing diagnostics.** There is no command to check database
   health, freelist pressure, or migration state without manual SQLite CLI use.

## Goals

- Pragmas are documented and enforced by mode (CLI vs server).
- Startup includes a lightweight integrity check.
- Operators can inspect database health via `aimee db` subcommands.

## Approach

### 1. Pragma profile table

Define explicit pragma profiles:

| Pragma | CLI | Server | Rationale |
|--------|-----|--------|-----------|
| `journal_mode` | WAL | WAL | Concurrent reads |
| `synchronous` | FULL | NORMAL | Server trades crash durability for throughput |
| `busy_timeout` | 5000 | 5000 | Prevent immediate BUSY failures |
| `foreign_keys` | ON | ON | Referential integrity |
| `cache_size` | -2048 (2MB) | -8192 (8MB) | Server handles more concurrent queries |
| `mmap_size` | 0 | 67108864 (64MB) | Server benefits from mmap; CLI is short-lived |
| `wal_autocheckpoint` | 1000 (default) | 1000 | Prevent unbounded WAL growth |

Apply via a `db_apply_pragmas(db, mode)` function called from both `db_open()`
and server initialization.

### 2. Startup self-check

Run `PRAGMA quick_check` during `session-start` (as already proposed in
`migration-recovery.md`). This proposal focuses on the pragma profile and
diagnostics; the recovery flow is in the migration-recovery proposal.

### 3. Operator health commands

Add `aimee db status`:

```
Schema version: 34
Journal mode:   wal
Synchronous:    normal
Page size:      4096
Page count:     1247
Freelist count: 12 (0.96%)
WAL size:       48 pages
Quick check:    ok
```

Add `aimee db pragma` to show all current pragma values for debugging.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Extract `db_apply_pragmas(db, mode)` with profile table, call from `db_open()` and server init |
| `src/db.c` | Add explicit `PRAGMA synchronous` and `PRAGMA wal_autocheckpoint` |
| `src/cmd_core.c` | Add `aimee db status` and `aimee db pragma` subcommands |
| `src/cmd_table.c` | Register new subcommands |

## Acceptance Criteria

- [ ] `PRAGMA synchronous` is explicitly set in both CLI and server modes
- [ ] `db_apply_pragmas()` is the single point of pragma configuration
- [ ] `aimee db status` reports schema version, journal mode, page stats, quick_check
- [ ] `aimee db pragma` dumps all configured pragma values
- [ ] Server mode uses NORMAL synchronous; CLI uses FULL

## Owner and Effort

- **Owner:** TBD
- **Effort:** S
- **Dependencies:** Migration recovery proposal (for `quick_check` integration)

## Rollout and Rollback

- **Rollout:** Ships with next binary. Pragma changes take effect on next `db_open()`.
- **Rollback:** Revert commit. Pragmas revert to current implicit behavior.
- **Blast radius:** Changing `synchronous` from FULL to NORMAL in server mode means a crash during a write could lose the most recent transaction. This is acceptable for a server that is not the primary data store (it can re-derive from agent output). CLI remains FULL.

## Test Plan

- [ ] Unit test: `db_apply_pragmas()` sets expected pragmas for each mode
- [ ] Integration test: `aimee db status` returns valid output on healthy database
- [ ] Integration test: `aimee db pragma` lists all configured pragmas
- [ ] Manual: verify WAL mode active and checkpoint behavior under concurrent access

## Operational Impact

- **Metrics:** None new.
- **Logging:** None new.
- **Alerts:** None.
- **Disk/CPU/Memory:** Explicit `wal_autocheckpoint` prevents unbounded WAL growth. No other resource impact.

## Priority

P0 — prevents silent pragma drift and provides diagnostic visibility.

## Trade-offs

**Why NORMAL sync in server mode?** Server mode prioritizes throughput for
concurrent agent operations. The data is reconstructible from agent output. CLI
uses FULL because it handles direct user operations where durability matters more.

**Why not `PRAGMA integrity_check`?** Full integrity check scans every page — too
slow for startup. `quick_check` verifies B-tree structure in milliseconds.
