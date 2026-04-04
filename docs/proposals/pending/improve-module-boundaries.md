# Proposal: Improve Module Boundaries and Reduce God Files

## Problem

Several SOLID-like architectural issues reduce maintainability:

1. **`memory.h` (372 lines, ~60 function declarations) violates Interface Segregation.** It declares CRUD, search, conflict detection, graph traversal, embeddings, content safety, context assembly, effectiveness tracking, memory linking, and drift detection. Any consumer that needs `memory_insert()` also pulls in `cosine_similarity()` and `memory_graph_prune()`. The implementation is already split across 6 files (`memory.c`, `memory_advanced.c`, `memory_promote.c`, `memory_context.c`, `memory_scan.c`, `memory_graph.c`) but the header is monolithic.

2. **God files violate Single Responsibility:**
   - `webchat.c` (2622 lines): HTML/CSS/JS templates, HTTP handling, WebSocket logic, and claude stream management
   - `cmd_core.c` (2280 lines): At least 18 distinct command handlers (init, setup, describe, feedback, mode, plan, dashboard, webchat, env, manifest, dispatch, export, import, db, status, usage, config, worktree)
   - `cmd_agent.c` (1915 lines): agent list, add, remove, test, run, dispatch, describe commands plus agent routing logic
   - `agent_tools.c` (1825 lines): Tool execution, sandboxing, checkpoints, all individual tool implementations

3. **Server dispatch is a 50-line if-else chain** (`server.c:480-530`). Adding a new server method requires modifying this chain. Violates Open/Closed.

4. **Regex patterns compiled on every call** in `memory.c` and `guardrails.c` (~10 instances). `regcomp()` is expensive and these patterns are static constants.

5. **`db_open_fast()` (`db.c:1197-1232`) skips FTS table creation.** Server connections use `db_open_fast()` for speed, but after a fresh install or schema upgrade, FTS queries will fail on these connections until a full `db_open()` runs.

## Goals

- `memory.h` split into focused sub-headers without breaking backward compatibility
- Largest files broken into cohesive modules under 1500 lines each
- Server dispatch uses a table instead of if-else
- Static regex patterns compiled once at init
- `db_open_fast()` guarantees FTS availability

## Approach

### Item 1: Split `memory.h` (non-breaking)

Create focused headers that each contain a subset of declarations. Keep `memory.h` as an umbrella that `#include`s all of them for backward compatibility:

| New Header | Contents |
|------------|----------|
| `memory_core.h` | `memory_t`, insert/get/delete/list/touch/stats |
| `memory_search.h` | search, find_facts, scan, compact, context assembly |
| `memory_lifecycle.h` | promote/demote/expire/maintain/health + lifecycle types |
| `memory_safety.h` | gate_check, scan_content, retention, ephemeral |
| `memory_links.h` | link CRUD + `memory_link_t` |
| `memory_effectiveness.h` | snapshots, outcomes, effectiveness stats |

`memory_graph.h` already exists as a separate header.

### Item 2: Split god files

| Current File | Split Into | Lines |
|-------------|-----------|-------|
| `webchat.c` (2622) | `webchat.c` (handler logic) + `webchat_assets.c` (HTML/CSS/JS string literals) | ~1300 + ~1300 |
| `cmd_core.c` (2280) | `cmd_core.c` (init, setup, env, status, config, usage) + `cmd_data.c` (export, import, db, manifest) + `cmd_session.c` (feedback, mode, plan, describe, worktree) | ~800 + ~700 + ~780 |
| `cmd_agent.c` (1915) | `cmd_agent.c` (list, add, remove, test) + `cmd_agent_run.c` (run, dispatch, describe) | ~900 + ~1000 |
| `agent_tools.c` (1825) | `agent_tools.c` (dispatch, sandbox, checkpoint) + `agent_tools_impl.c` (individual tool implementations) | ~500 + ~1300 |

### Item 3: Server dispatch table

Replace the if-else chain in `server.c:480-530` with:
```c
typedef int (*method_handler_t)(server_ctx_t *, server_conn_t *, cJSON *);

static const struct {
    const char *method;
    method_handler_t handler;
} dispatch_table[] = {
    {"memory.search", handle_memory_search},
    {"memory.store",  handle_memory_store},
    {"delegate",      handle_delegate},
    // ...
    {NULL, NULL}
};
```
Lookup via linear scan (table is <30 entries, so O(n) is fine).

### Item 4: Precompile regex patterns

Add initialization functions called once at startup:
- `memory_regex_init()` — compiles the ~6 patterns used in `gate_check_sensitive`, `gate_check_ephemeral`, `gate_has_evidence_markers`
- `guardrails_regex_init()` — compiles the `scan_rules[]` patterns in `memory_scan_content`

Store compiled `regex_t` in file-scoped static globals. The ~10 `regcomp()` calls become direct `regexec()` calls.

### Item 5: Fix `db_open_fast()` FTS gap

After the fast-path pragma setup in `db_open_fast()`, add a check for FTS table existence:
```c
sqlite3_stmt *chk = NULL;
rc = sqlite3_prepare_v2(db,
    "SELECT 1 FROM sqlite_master WHERE type='table' AND name='memories_fts'",
    -1, &chk, NULL);
int has_fts = (rc == SQLITE_OK && sqlite3_step(chk) == SQLITE_ROW);
if (chk) sqlite3_finalize(chk);

if (!has_fts) {
    sqlite3_close(db);
    return db_open(path);  // Fall back to full open with FTS creation
}
```

### Changes

| File | Change |
|------|--------|
| `src/headers/memory_core.h` | New: core CRUD declarations |
| `src/headers/memory_search.h` | New: search/scan declarations |
| `src/headers/memory_lifecycle.h` | New: promote/demote/maintain declarations |
| `src/headers/memory_safety.h` | New: gate/scan/retention declarations |
| `src/headers/memory_links.h` | New: link CRUD declarations |
| `src/headers/memory_effectiveness.h` | New: effectiveness tracking declarations |
| `src/headers/memory.h` | Keep as umbrella, `#include` all sub-headers |
| `src/webchat_assets.c` | New: extracted HTML/CSS/JS literals |
| `src/webchat.c` | Remove template strings, include `webchat_assets.c` definitions |
| `src/cmd_data.c` | New: export, import, db, manifest handlers |
| `src/cmd_session.c` | New: feedback, mode, plan, describe, worktree handlers |
| `src/cmd_core.c` | Reduced to init, setup, env, status, config, usage |
| `src/cmd_agent_run.c` | New: run, dispatch, describe handlers |
| `src/cmd_agent.c` | Reduced to list, add, remove, test |
| `src/agent_tools_impl.c` | New: individual tool implementations |
| `src/agent_tools.c` | Reduced to dispatch, sandbox, checkpoint |
| `src/server.c` | Replace if-else dispatch with `dispatch_table[]` |
| `src/memory.c` | Add `memory_regex_init()`, precompile all patterns |
| `src/guardrails.c` | Add `guardrails_regex_init()`, precompile scan_rules patterns |
| `src/db.c` | Add FTS existence check to `db_open_fast()` |
| `Makefile` | Add new `.c` files to build |

## Acceptance Criteria

- [ ] No source file exceeds 1500 lines
- [ ] `#include "memory.h"` still compiles all existing code (umbrella backward compat)
- [ ] Consumers can include only `memory_core.h` for CRUD operations
- [ ] Server dispatch table replaces the if-else chain
- [ ] `regcomp()` called ≤1 time per pattern across entire process lifetime
- [ ] `db_open_fast()` connections can execute FTS queries immediately
- [ ] All existing tests pass
- [ ] Build time not significantly impacted (<5% increase)

## Owner and Effort

- **Owner:** aimee core
- **Effort:** L
- **Dependencies:** Coordinate with DRY proposal (shared utilities). Header split should land before file splits to avoid merge conflicts.

## Rollout and Rollback

- **Rollout:** Incremental — each item can land as a separate commit. Recommended order: Item 5 (small fix) → Item 4 (small fix) → Item 3 (dispatch table) → Item 1 (header split) → Item 2 (file splits)
- **Rollback:** Revert individual commits
- **Blast radius:** Header split affects all consumers (but umbrella maintains compat). File splits affect build system. Dispatch table affects all server methods. FTS fix affects server startup path.

## Test Plan

- [ ] Unit tests: existing test suite passes after each item
- [ ] Integration tests: full delegation round-trip, memory CRUD, server method routing
- [ ] Failure injection: `db_open_fast()` on fresh database confirms FTS fallback works
- [ ] Manual verification: `wc -l` confirms no file exceeds 1500 lines

## Operational Impact

- **Metrics:** None
- **Logging:** None
- **Disk/CPU/Memory:** Precompiled regex uses ~10KB permanent memory (worthwhile given call frequency). FTS check adds one lightweight query to `db_open_fast()`.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Fix `db_open_fast()` FTS gap | P2 | S | Prevents FTS query failures on server connections |
| Precompile regex patterns | P2 | S | Performance improvement for hot paths |
| Server dispatch table | P3 | S | Maintainability, Open/Closed compliance |
| Split `memory.h` into sub-headers | P3 | M | Interface Segregation, reduced coupling |
| Split god files | P3 | L | Single Responsibility, navigability |

## Trade-offs

- The header split adds more files but reduces coupling. The umbrella header means existing code doesn't break.
- The file splits require updating the Makefile for new `.c` files and may cause short-term merge conflicts with in-flight work.
- Dispatch table has O(n) lookup vs O(1) if-else — but n<30, so the difference is unmeasurable.
- Precompiling regex uses ~10KB of permanent memory for the compiled patterns. Worthwhile given the frequency of calls.
- An alternative to splitting `cmd_core.c` is using a command sub-dispatch table within the file, but the file would still be 2280 lines — the problem is size, not just dispatch.
