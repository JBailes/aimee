# Proposal: Server-Side Caching Layer

## Problem

The aimee server re-computes stable results on every request. Profiling shows four categories of redundant work:

1. **`config_load()` / `agent_load_config()`** — re-reads and re-parses JSON from disk on every call. `config_load()` is called 20+ times across the codebase per request cycle (`cmd_hooks.c`, `server_compute.c`, `server.c`, `guardrails.c`, `agent_context.c`, etc.). The files almost never change.

2. **`rules_generate()`** — queries the rules table and formats markdown on every call, including every `agent_build_exec_context()`. Rules change only on manual user edits. Cost: 10-50ms per call.

3. **`memory_assemble_context()`** — runs 4 SQL queries per call (L2 facts, entity edges, etc.). The infrastructure to cache this already exists — `cache_get()`, `cache_put()`, `cache_invalidate()` in `memory_context.c:321-376` and the `context_cache` table in `db.c:304-308` — but nothing calls them.

4. **SQLite page cache** — default is ~2MB (500 pages × 4KB). The server's long-lived DB connection would benefit from a larger cache and memory-mapped I/O to avoid `read()` syscalls on hot pages.

## Goals

- Eliminate redundant file I/O for config loading on the server.
- Cache `rules_generate()` output with automatic invalidation on rules mutations.
- Activate the existing (dormant) `context_cache` for `memory_assemble_context()`.
- Reduce SQLite I/O overhead for the server's persistent DB connection.

## Approach

### Cache `config_load()` / `agent_load_config()` with file mtime check (S)

Add a static in-process cache that returns the previously-loaded config when the file's mtime hasn't changed.

```c
static config_t g_config_cache = {0};
static time_t g_config_mtime = 0;

int config_load(config_t *cfg) {
    struct stat st;
    if (stat(path, &st) == 0 && st.st_mtime == g_config_mtime && g_config_cache.loaded) {
        memcpy(cfg, &g_config_cache, sizeof(*cfg));
        return 0;
    }
    /* ... normal load, then update cache ... */
}
```

| File | Change |
|------|--------|
| `src/config.c` | Add static cache + mtime guard to `config_load()` |
| `src/agent_config.c` | Same pattern for `agent_load_config()` |

### Cache `rules_generate()` output (S)

Cache key: `hash(COUNT(*) || MAX(updated_at))` from the rules table. Invalidate on any write to rules. The `context_cache` table already exists for this purpose.

| File | Change |
|------|--------|
| `src/rules.c` | Add cache check at top of `rules_generate()`; invalidate on `rules_insert/update/delete` |

### Activate dormant `context_cache` for `memory_assemble_context()` (S)

Wire the existing `cache_get()`/`cache_put()` calls into `memory_assemble_context()`. Cache key: `cache_input_hash()` already exists at `memory_context.c:379` — combines memory count + max `updated_at`. TTL: `CACHE_TTL_SECONDS = 300` (5 minutes, already defined).

**Write-through invalidation:** `memory_store()` and `memory_update()` must call `cache_invalidate()` after writing, so the next `memory_assemble_context()` call rebuilds from fresh data rather than serving stale context for up to 5 minutes. This is critical because context may contain policy-relevant rules or security-sensitive facts.

| File | Change |
|------|--------|
| `src/memory_context.c` | Wire `cache_get()`/`cache_put()` into `memory_assemble_context()` |
| `src/memory.c` | Call `cache_invalidate()` after `memory_store()` and `memory_update()` |

### Increase SQLite page cache and enable mmap (S)

Tune PRAGMAs for the server's long-lived DB connection:

```c
/* In server_init() after db_open() */
sqlite3_exec(db, "PRAGMA cache_size = -8192", NULL, NULL, NULL);  /* 8MB */
sqlite3_exec(db, "PRAGMA mmap_size = 67108864", NULL, NULL, NULL); /* 64MB mmap */
```

`PRAGMA mmap_size` lets SQLite memory-map the database file, avoiding `read()` syscalls for cached pages. Combined with WAL mode (already enabled), this makes read-heavy workloads nearly zero-copy.

| File | Change |
|------|--------|
| `src/server.c` | Add `PRAGMA cache_size` and `PRAGMA mmap_size` after `db_open()` in `server_init()` |

## Acceptance Criteria

- [ ] `config_load()` does not re-read file when mtime is unchanged (verified via `strace -e openat`)
- [ ] `rules_generate()` returns cached result when rules table is unchanged (verified via sqlite3 trace showing no SELECT on rules)
- [ ] `memory_assemble_context()` returns cached result within TTL window
- [ ] `memory_store()` invalidates context cache — next `memory_assemble_context()` call rebuilds from DB
- [ ] `AIMEE_NO_CACHE=1` disables all caches — every call hits source (verified via strace/sqlite3 trace)
- [ ] Server SQLite page cache set to 8MB (`PRAGMA cache_size` verified via `PRAGMA cache_size` query)
- [ ] All existing integration tests pass without modification
- [ ] No regression in `make test` suite

## Owner and Effort

- **Owner:** JBailes
- **Effort:** M (four S changes, but testing cache invalidation correctness across all paths adds integration effort)
- **Dependencies:** None — works independently of other proposals. Compounds well with the CLI thin-client proposal (cached server responses make RPC-routed calls even faster).

## Rollout and Rollback

- **Rollout:** Direct code changes. Each cache is a separate commit. All caches respect the `AIMEE_NO_CACHE` environment variable as a kill-switch: when set, all caches are bypassed and every call goes to the source (file/DB). This allows diagnosing cache-related bugs in production without redeploying.
- **Rollback:** `git revert` of any individual commit. No migrations or state changes. Reverting removes caching — performance degrades but correctness is unaffected.
- **Blast radius:** Config cache affects all server request handling. Rules cache affects agent context assembly. Context cache affects memory assembly. SQLite PRAGMAs affect all DB queries on the server. All changes are server-side only — no CLI impact.

**Staged rollout:** Enable caches one at a time (separate commits). Order: config mtime cache (safest, no DB involvement) → SQLite PRAGMAs (no application logic) → rules cache → context cache (highest staleness risk, last). Each commit must pass the full test suite including cache-specific integration tests before the next is merged.

## Test Plan

- [ ] Unit tests: `config_load()` cache returns same result on second call without file change
- [ ] Unit tests: `config_load()` cache invalidates when file mtime changes
- [ ] Unit tests: `rules_generate()` cache invalidates after `INSERT INTO rules`
- [ ] Unit tests: `context_cache` hit returns stored output within TTL, miss after TTL expires
- [ ] Integration tests: server with mmap + large cache handles concurrent requests without contention
- [ ] Integration test: after server startup, second `config_load()` call does not open config.json (verified via test hook or counter)

## Operational Impact

- **Metrics:** No new metrics.
- **Logging:** Cache miss on `rules_generate()` logs at debug level.
- **Alerts:** None.
- **Disk/CPU/Memory:** Server memory increases ~8-70MB (8MB SQLite page cache + up to 64MB mmap region). mmap is virtual memory — actual RSS depends on which pages are accessed.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Config file mtime cache | P1 | S | Eliminates 20+ file reads per request cycle |
| Rules output cache | P1 | S | Saves 10-50ms per agent context build |
| Activate context_cache | P1 | S | Saves 20-50ms per memory assembly (4 queries) |
| SQLite page cache + mmap | P2 | S | Reduces syscall overhead for all DB queries |

## Trade-offs

**Caching staleness:** `config_load()` cache uses file mtime — sub-second edits within the same second won't invalidate. Acceptable because config edits are manual and rare. `rules_generate()` cache uses `COUNT(*) + MAX(updated_at)` — covers all mutation paths since rules use `datetime('now')` defaults. `context_cache` uses write-through invalidation (invalidated on `memory_store()`/`memory_update()`) plus a 5-minute TTL as a safety net. The TTL only matters if an invalidation call is missed — the write-through path ensures freshness for the normal case.

**SQLite mmap:** `PRAGMA mmap_size = 64MB` maps the database file into the server's address space. Eliminates `read()` syscalls for cached pages but increases virtual memory usage. If the database grows beyond 64MB, only the first 64MB is mapped; the rest falls back to normal I/O. On memory-constrained systems, the mmap region competes with other allocations. The 8MB page cache is the safer baseline; mmap is additive.

**Dormant infrastructure risk:** The `context_cache` code in `memory_context.c` has never been exercised in production. It may have bugs or edge cases that only appear under real workloads. Mitigated by the existing test infrastructure and the fact that cache misses fall through to the uncached path.
