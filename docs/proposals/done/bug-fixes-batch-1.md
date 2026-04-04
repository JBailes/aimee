# Proposal: Bug Fixes Batch 1: Resource Leaks, NULL Derefs, and Safety Issues

## Problem

A comprehensive codebase audit found 12 bugs spanning resource leaks, null pointer dereferences, use-after-free risks, thread safety issues, and non-functional code paths. Each bug is independently verifiable and fixable.

## Goals

- All 12 identified bugs fixed with no regressions.
- All existing tests continue to pass.
- Each fix is minimal and isolated to prevent collateral damage.

## Approach

### Bug 1: Pipe leak in cmd_chat.c (~line 818)

If `pipe(out_pipe)` fails after `pipe(in_pipe)` succeeds, the two in_pipe file descriptors are never closed.

**Fix:** Close in_pipe[0] and in_pipe[1] before the early return when out_pipe fails.

### Bug 2: Signal handler never installed in cmd_chat.c (~line 30)

`chat_sigint_handler` is declared and `chat_interrupted` is checked throughout the file, but `signal(SIGINT, chat_sigint_handler)` is never called. The interrupt mechanism is non-functional.

**Fix:** Register the signal handler at the start of cmd_chat().

### Bug 3: strtok() reentrancy in text.c and render.c

`strtok()` is used in `canonical_fingerprint` (text.c:192), `word_similarity` (text.c:241), and `filter_fields` (render.c:66). If any called function (e.g., `stem_word`) internally uses `strtok()`, the outer tokenization state is corrupted.

**Fix:** Replace all `strtok()` calls with `strtok_r()`.

### Bug 4: Memory leak in agent_policy.c (~line 194-217)

In `policy_check_tool()`, a cJSON `args` object is parsed but only freed inside a conditional block. When `cmd` is NULL, the early path skips `cJSON_Delete(args)`.

**Fix:** Move `cJSON_Delete(args)` to execute unconditionally after the conditional block.

### Bug 5: Memory leak in agent_policy.c (~line 224-284)

A second `args` object is parsed for path-prefix rule checking. The `continue` at line ~246 skips `cJSON_Delete(args)` at line ~283.

**Fix:** Add `cJSON_Delete(args)` before the `continue` statement.

### Bug 6: Memory leak in memory_context.c (~line 487)

`query_embed` is malloc'd but never freed when the embedding model is unavailable and the function continues past the error.

**Fix:** Add `free(query_embed)` in the error path.

### Bug 7: Resource leak in memory_scan.c (~line 362-476)

When the window_id INSERT fails (window_id remains 0), the `terms` and `file_refs` arrays are not freed because cleanup is inside the `if (window_id > 0)` block.

**Fix:** Move the cleanup loops outside the conditional.

### Bug 8: Unfinalized SQLite statement in cmd_core.c (~line 209-227)

The `quick_check` PRAGMA statement is finalized late (after multiple printf calls). If the function exits early, the statement leaks.

**Fix:** Finalize immediately after extracting the result.

### Bug 9: NULL dereference in cmd_describe.c (~line 138)

`strrchr(line, '/') + 1` produces undefined behavior if `strrchr` returns NULL (no slash in line). The variable `slash` is checked but the wrong `strrchr` call is used.

**Fix:** Use the already-checked `slash` variable instead of calling `strrchr` again.

### Bug 10: Unchecked strdup() returns in agent.c (~lines 144, 156, 231, 346)

`strdup()` return values are used without NULL checks. If malloc fails internally, subsequent code dereferences NULL.

**Fix:** Add NULL checks after each strdup() call.

### Bug 11: Session lookup race in webchat.c (~lines 708-740)

`session_lookup()` returns a pointer to a session after releasing the `sessions_mutex`. Another thread could evict the session before the caller acquires `session->lock`, causing use-after-free.

**Fix:** Use reference counting on sessions. Increment refcount under the mutex in session_lookup, decrement after use. Only evict sessions with refcount 0.

### Bug 12: Statement cache thread safety in db.c (~lines 27-62)

Two threads calling `db_prepare()` with the same SQL receive the same cached statement. Thread B's `sqlite3_reset()` can interrupt Thread A's iteration.

**Fix:** Add a comment documenting that the statement cache is designed for single-threaded use (which matches current usage: server uses one thread per request with its own db connection). If multi-threaded use is needed later, switch to per-connection caching.

### Changes

| File | Change |
|------|--------|
| `src/cmd_chat.c` | Close in_pipe fds on pipe failure; register SIGINT handler |
| `src/text.c` | Replace strtok() with strtok_r() |
| `src/render.c` | Replace strtok() with strtok_r() |
| `src/agent_policy.c` | Fix two cJSON memory leaks |
| `src/memory_context.c` | Free query_embed on error path |
| `src/memory_scan.c` | Move cleanup outside conditional |
| `src/cmd_core.c` | Finalize quick_check stmt immediately |
| `src/cmd_describe.c` | Fix NULL check for strrchr |
| `src/agent.c` | Add NULL checks after strdup() |
| `src/webchat.c` | Add session reference counting |
| `src/db.c` | Document statement cache threading model |

## Acceptance Criteria

- [ ] Each of the 12 bugs is fixed with a targeted, minimal change
- [ ] All existing unit tests pass
- [ ] All existing integration tests pass
- [ ] No new compiler warnings with -Wall -Wextra -Werror
- [ ] cppcheck passes with no new warnings
- [ ] clang-tidy passes

## Owner and Effort

- **Owner:** TBD
- **Effort:** M (12 isolated fixes, each small, but testing across many files)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change. Each fix is independently revertable.
- **Rollback:** git revert. Each fix is isolated.
- **Blast radius:** Low per fix. Bug 11 (session refcounting) has the widest blast radius (webchat only).

## Test Plan

- [ ] Unit test: strtok_r replacement produces identical tokenization results
- [ ] Unit test: signal handler is registered (mock test or manual)
- [ ] Integration tests: all existing tests pass unchanged
- [ ] Manual verification: each bug's specific scenario no longer triggers
- [ ] Valgrind/ASAN: run test suite under sanitizers to verify leak fixes

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** Slight reduction in memory leaks under error conditions.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Bug fixes batch 1 | P1 | M | Correctness, safety, prevents crashes |

## Trade-offs

**Why batch instead of individual PRs?** Each fix is 5-20 lines. Individual PRs would create excessive review overhead for trivial changes. The batch groups them logically while keeping each fix isolated in its own commit.

**Why document db.c threading instead of fixing it?** The current server architecture uses one db connection per request, so the cache is safe in practice. A full fix (per-connection caching) is a larger refactor that should be its own proposal if multi-threaded DB access is needed.
