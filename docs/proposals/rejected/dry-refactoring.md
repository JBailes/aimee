# Proposal: DRY Refactoring: Extract Common Patterns and String Builder

## Problem

The codebase has several patterns duplicated extensively:

1. **db_prepare/bind/step boilerplate**: The pattern `db_prepare(db, sql); if(!stmt) return; while(sqlite3_step(stmt)==SQLITE_ROW) { extract columns; } sqlite3_reset(stmt);` repeats 30+ times across memory.c, memory_promote.c, memory_context.c, memory_advanced.c, tasks.c, working_memory.c, dashboard.c, and server_state.c.

2. **Subcommand dispatch boilerplate**: The pattern `open db; parse sub from argv; subcmd_dispatch(); close db;` repeats in cmd_db, cmd_worktree, cmd_memory, cmd_index, cmd_wm with minor variations.

3. **Config string loading**: The pattern `cJSON_GetObjectItem(root, key); if(cJSON_IsString(item)) snprintf(dst, len, item->valuestring)` repeats 8+ times in config.c:265-310.

4. **Dashboard API functions**: Six `api_*` functions in dashboard.c follow identical structure (prepare, loop, extract, build JSON array, return).

5. **sqlite3_column_text NULL guard**: The pattern `(const char *)sqlite3_column_text(stmt, N); snprintf(dst, len, "%s", val ? val : "")` repeats in every row extraction function.

6. **Repeated `pos += snprintf(buf + pos, cap - pos, ...)` buffer assembly**: Used extensively in `memory_context.c`, `agent_context.c`, `cmd_hooks.c`, `cmd_chat.c`, and `webchat.c`. This pattern is error-prone: if snprintf returns a value exceeding remaining capacity, `pos` can drift beyond `cap`, causing undefined behavior in subsequent writes. The pattern is duplicated and difficult to reason about in review.

7. **Duplicate error-message scaffolding in agent.c**: Near-identical blocks for tool validation/policy/directive failures and protocol-specific response packaging repeat 3 times (~75 lines each).

## Goals

- Reduce total codebase LOC by ~500-800 lines through shared helpers.
- Each duplicated pattern extracted into a reusable function or macro.
- Introduce bounded string-builder primitives to remove repeated ad-hoc buffer math.
- No functional changes.

## Approach

### 1. String Builder Helper (saves ~200 lines, eliminates truncation bugs)

```c
/* src/headers/string_builder.h */
typedef struct {
   char *buf;
   size_t cap;
   size_t pos;
} sb_t;

void sb_init(sb_t *sb, char *buf, size_t cap);
int  sb_append(sb_t *sb, const char *fmt, ...);  /* returns 0 on success, -1 if truncated */
int  sb_remaining(const sb_t *sb);
```

Place in `src/util.c` and `src/headers/util.h`. Guarantees NUL-termination and clamps position. Replaces ad-hoc `pos += snprintf(buf + pos, cap - pos, ...)` accumulation in:
- `memory_context.c` (context assembly)
- `agent_context.c` (exec context assembly, 400-line function)
- `cmd_hooks.c` (session context builder)
- `cmd_chat.c` (prompt building)
- `webchat.c` (response building)

### 2. DB Query Helper (saves ~300 lines)

```c
typedef int (*db_row_fn)(sqlite3_stmt *stmt, void *ctx, int row_idx);
int db_query_each(sqlite3 *db, const char *sql, db_row_fn fn, void *ctx, int max_rows);
```

Place in `src/db.c`. Encapsulates the prepare/step/reset boilerplate. The callback receives each row and populates caller-owned structures.

### 3. Safe Column Text Macro (saves ~50 lines)

```c
#define COL_TEXT(stmt, i) \
   ({ const char *_v = (const char *)sqlite3_column_text(stmt, i); _v ? _v : ""; })
```

Place in `src/headers/db.h`. Replaces the ternary NULL guards repeated in every row extraction.

### 4. Config String Loader (saves ~60 lines)

```c
static void config_load_str(cJSON *root, const char *key, char *dst, size_t len);
```

Place in `src/config.c`. Replaces 8+ identical JSON-to-buffer copies.

### 5. Subcommand Dispatch Helper (saves ~40 lines)

```c
void cmd_subcmd_with_db(app_ctx_t *ctx, int argc, char **argv,
                        const char *name, const subcmd_t *table, int full_open);
```

Place in `src/cmd_util.c`. Replaces repeated open/dispatch/close pattern in cmd_db, cmd_worktree, cmd_memory, cmd_index, cmd_wm.

### 6. Tool Error Result Helper (saves ~150 lines)

```c
void tool_error_result(cJSON *messages, cJSON *results,
                       const parsed_tool_call_t *call,
                       const char *error_msg, int provider_format);
```

Place in `src/agent.c`. Replaces the three near-identical error scaffolding blocks for validation/policy/directive failures.

### Changes

| File | Change |
|------|--------|
| `src/util.c` | Add sb_init, sb_append, sb_remaining |
| `src/headers/util.h` | Add sb_t typedef and declarations |
| `src/db.c` | Add db_query_each helper |
| `src/headers/db.h` | Add COL_TEXT macro, db_query_each declaration |
| `src/config.c` | Add config_load_str, refactor config_load |
| `src/cmd_util.c` | Add cmd_subcmd_with_db helper |
| `src/headers/commands.h` | Add cmd_subcmd_with_db declaration |
| `src/agent.c` | Add tool_error_result, refactor error blocks |
| `src/memory.c` | Use db_query_each where applicable |
| `src/memory_promote.c` | Use db_query_each |
| `src/memory_context.c` | Use sb_t for context assembly |
| `src/agent_context.c` | Use sb_t for exec context assembly |
| `src/cmd_hooks.c` | Use sb_t for session context |
| `src/tasks.c` | Use db_query_each and COL_TEXT |
| `src/working_memory.c` | Use db_query_each and COL_TEXT |
| `src/dashboard.c` | Use db_query_each for api_* functions |
| `src/cmd_core.c` | Use cmd_subcmd_with_db for cmd_db, cmd_worktree |
| `src/cmd_memory.c` | Use cmd_subcmd_with_db |

## Acceptance Criteria

- [ ] All 6 shared helpers implemented and used
- [ ] sb_t used in at least 5 buffer-assembly callsites
- [ ] db_query_each used in at least 10 query sites
- [ ] Buffer assembly in context/chat/webchat paths uses sb_t
- [ ] Unit tests for sb_append boundary conditions (near-capacity, exact-capacity, over-capacity)
- [ ] Unit tests for db_query_each (empty result, max_rows limit, callback error)
- [ ] No functional changes to any command output
- [ ] All existing tests pass

## Owner and Effort

- **Owner:** TBD
- **Effort:** M-L (many files touched, but each change is mechanical)
- **Dependencies:** Should land after file-splitting proposals to avoid merge conflicts

## Rollout and Rollback

- **Rollout:** Direct code change. Can be done incrementally (one helper at a time).
- **Rollback:** git revert per helper.
- **Blast radius:** Widespread but mechanical. Each callsite replacement is independently correct.

## Test Plan

- [ ] Unit test: sb_append with boundary conditions (empty, near-cap, at-cap, over-cap)
- [ ] Unit test: sb_append returns -1 on truncation
- [ ] Unit test: db_query_each with 0 rows, N rows, max_rows limit
- [ ] Unit test: COL_TEXT with NULL and non-NULL values
- [ ] Integration tests pass unchanged
- [ ] Manual: verify context assembly output is identical before/after

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** Negligible. sb_t adds one struct on the stack per builder site.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| String builder (sb_t) | P1 | S | Eliminates truncation bugs |
| DB query helper | P2 | M | Reduces 300+ lines of boilerplate |
| Other helpers | P2 | S each | Quality of life |

## Trade-offs

**Why a custom sb_t instead of using an existing library?** The codebase has no external dependencies beyond SQLite and cJSON. A 30-line helper is simpler than adding a dependency.

**Why db_query_each callback instead of a cursor/iterator?** Callbacks match the existing code pattern (process rows inline) and avoid introducing new control flow abstractions. The callback also handles cleanup automatically.

**Why COL_TEXT as a macro instead of a function?** The GCC statement expression syntax avoids a function call per column access. This is used in hot loops processing hundreds of rows.
