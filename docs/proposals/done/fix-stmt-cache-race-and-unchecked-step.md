# Proposal: Fix Statement Cache Race and Unchecked SQLite Step Calls

## Problem

Two categories of SQLite-related bugs exist in the codebase:

1. **HIGH — `db.c:1238`:** `db_close()` calls `db_stmt_cache_clear()` which finalizes **all** cached statements across **all** connections. The statement cache is global and mutex-protected, but `db_close()` destroys statements belonging to other open connections. If thread A closes its db connection while thread B is mid-query using a cached statement from a different connection, thread B has a use-after-free. The correct function `db_stmt_cache_clear_for(db)` already exists at `db.c:86` and only clears statements for the specified connection — but `db_close()` doesn't use it.

2. **LOW-MEDIUM — 160 instances across 35 files** where `sqlite3_step()` return value is ignored (fire-and-forget pattern). Most are `INSERT`/`UPDATE` calls for provenance logging, metrics, and health tracking. Individual failures are tolerable but cause silent data loss with no diagnostic. Files with the most ignored returns: `memory_promote.c` (23), `index.c` (14), `memory.c` (9), `memory_scan.c` (8), `memory_advanced.c` (7), `tasks.c` (6), `cmd_work.c` (6).

## Goals

- `db_close()` only clears statements for its own connection
- Critical write paths check `sqlite3_step()` return values
- Non-critical write paths at minimum log failures at debug level

## Approach

**Bug 1 fix (one-line change):**

In `db_close()` at `db.c:1238`, change:
```c
db_stmt_cache_clear();    // BAD: clears ALL connections
```
to:
```c
db_stmt_cache_clear_for(db);  // GOOD: clears only this connection
```

**Bug 2 fix (incremental):**

Add a `DB_STEP_LOG(stmt, label)` macro that checks the return value and logs on failure:
```c
#define DB_STEP_LOG(stmt, label) do { \
    int _rc = sqlite3_step(stmt); \
    if (_rc != SQLITE_DONE && _rc != SQLITE_ROW) \
        log_warn("sqlite3_step failed in %s: %s", label, \
                 sqlite3_errmsg(sqlite3_db_handle(stmt))); \
} while(0)
```

Apply to all 160 call sites incrementally, prioritizing `memory_promote.c` and `memory.c` first. For truly fire-and-forget paths (provenance, health metrics), the macro provides diagnostics without changing behavior.

### Changes

| File | Change |
|------|--------|
| `src/db.c:1238` | Change `db_stmt_cache_clear()` to `db_stmt_cache_clear_for(db)` |
| `src/headers/db.h` | Add `DB_STEP_LOG` macro |
| `src/memory_promote.c` | Replace 23 bare `sqlite3_step()` calls with `DB_STEP_LOG` |
| `src/memory.c` | Replace 9 bare `sqlite3_step()` calls with `DB_STEP_LOG` |
| `src/index.c` | Replace 14 bare `sqlite3_step()` calls with `DB_STEP_LOG` |
| Remaining 32 files | Replace bare `sqlite3_step()` calls incrementally |

## Acceptance Criteria

- [ ] `db_close()` uses `db_stmt_cache_clear_for(db)`, not `db_stmt_cache_clear()`
- [ ] No use-after-free when closing one connection while another is active (tested with concurrent test)
- [ ] `DB_STEP_LOG` macro exists and is applied to at least `memory_promote.c` and `memory.c`
- [ ] `grep -c 'sqlite3_step(.*);$'` shows reduction from 160 to <10 (remaining are intentionally unchecked and documented)

## Owner and Effort

- **Owner:** aimee core
- **Effort:** S (bug 1), M (bug 2)
- **Dependencies:** none

## Rollout and Rollback

- **Rollout:** Bug 1 is a one-line fix, deploy immediately. Bug 2 is incremental, one file per commit.
- **Rollback:** Revert commit
- **Blast radius:** Bug 1 affects all server-mode usage with concurrent connections. Bug 2 affects diagnostics only (adds logging, no behavioral change).

## Test Plan

- [ ] Unit tests: concurrent test where one thread closes a DB while another uses cached statements on a different connection
- [ ] Integration tests: full test suite passes
- [ ] Failure injection: force `sqlite3_step()` failures and confirm `DB_STEP_LOG` emits warnings
- [ ] Manual verification: `grep` confirms bare `sqlite3_step()` count reduced

## Operational Impact

- **Metrics:** None
- **Logging:** `DB_STEP_LOG` adds warning-level logs for previously silent failures
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible. One branch per step call.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Fix `db_close()` statement cache | P1 | S | Prevents use-after-free in concurrent server workloads |
| Add `DB_STEP_LOG` macro | P3 | M | Improves diagnostics for silent write failures |

## Trade-offs

The `DB_STEP_LOG` macro adds a branch to every step call. In practice this is negligible since the `sqlite3_step()` call itself dominates. An alternative is wrapping `db_prepare` + bind + step into a higher-level API, but that's a larger refactor better handled as part of a broader DRY proposal for SQLite boilerplate.
