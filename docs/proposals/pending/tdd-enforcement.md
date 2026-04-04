# Proposal: Test-Driven Development Enforcement in Guardrails

## Problem

When delegates or the primary agent implement code, they often write the implementation first and tests second (or not at all). This leads to:
- Tests that are shaped around the implementation rather than the desired behavior
- Missing test coverage for edge cases discovered during implementation
- Tests that pass trivially because they were written to match existing code

Aimee's guardrails enforce sensitive file protection, plan mode, and anti-patterns, but do not enforce any development methodology constraints. There is no way to ensure tests are written before or alongside implementation.

oh-my-codex's `$tdd` skill enforces strict red-green-refactor: no production code without a failing test first. The enforcement is at the workflow level — it halts the agent if code precedes tests. While strict TDD isn't always appropriate, the insight that guardrails can enforce development methodology (not just safety) is powerful.

Evidence:
- `guardrails.c` has PostToolUse hooks that fire on every tool call — ideal for enforcement
- `git_verify.c` runs tests but doesn't check test-first ordering
- No mechanism tracks whether tests were written before implementation in a session
- Delegates can be asked to write tests but compliance is not enforced

## Goals

- An optional TDD enforcement mode that tracks write ordering within a session
- When enabled, implementation files written before their corresponding test files trigger a warning
- The enforcement is advisory by default, blocking only when explicitly configured
- The system learns test file conventions per project (e.g., `foo.c` → `tests/test_foo.c`)

## Approach

### 1. Test file mapping

Auto-detect test file conventions from the project structure:

```c
typedef struct {
    char pattern[128];      // e.g., "src/*.c"
    char test_pattern[128]; // e.g., "src/tests/test_*.c"
} test_mapping_t;

int detect_test_mappings(const char *project_root, test_mapping_t *out, int max);
char *find_test_file(const test_mapping_t *mappings, int count, const char *source_file);
```

Heuristics:
- Scan for existing test directories (`tests/`, `test/`, `__tests__/`, `spec/`)
- Match naming patterns (`test_*.c`, `*_test.go`, `*.test.ts`, `*_spec.rb`)
- Store learned mappings in working memory for the session

### 2. Write ordering tracking

In `post_tool_update()`, track file writes in session-scoped state:

```c
typedef struct {
    char path[512];
    int is_test;
    time_t written_at;
} write_record_t;
```

When an implementation file is written:
1. Find its corresponding test file using the mapping
2. Check if the test file was written *before* or *during the same session*
3. If not → surface a TDD advisory in the session context

### 3. TDD mode toggle

```bash
aimee tdd on            # enable TDD enforcement (advisory)
aimee tdd on --strict   # enable TDD enforcement (blocking — PreToolUse blocks impl writes without test)
aimee tdd off           # disable
```

Config: `aimee config set tdd.mode advisory|strict|off`

### 4. Session context injection

When TDD mode is active, add to session context:

```
# TDD Mode (advisory)
- src/foo.c written without corresponding test (expected: src/tests/test_foo.c)
- Suggestion: write the test first to define expected behavior
```

In strict mode, the PreToolUse hook for Edit/Write tools would block writes to implementation files unless the corresponding test file has been written in the current session. The block message explains what test file to write.

### 5. Integration with delegates

When delegating `code` tasks in TDD mode, automatically append to the delegation prompt:

```
TDD mode is active. Write tests first, then implementation. Test file convention: {pattern}
```

### Changes

| File | Change |
|------|--------|
| `src/guardrails.c` | Add write ordering tracking in `post_tool_update()`, TDD advisory injection |
| `src/cmd_hooks.c` | Add TDD check in PreToolUse for strict mode blocking |
| `src/config.c` | Add `tdd.mode` config option |
| `src/cmd_core.c` | Add `tdd` subcommand |
| `src/tests/test_tdd.c` | Tests for test file mapping, write ordering, strict blocking |

## Acceptance Criteria

- [ ] Test file mapping auto-detects conventions from project structure
- [ ] `aimee tdd on` enables advisory mode — warns when impl precedes tests
- [ ] `aimee tdd on --strict` blocks implementation writes without prior test writes
- [ ] Session context shows TDD violations
- [ ] Delegated code tasks include TDD prompt injection when TDD mode is active
- [ ] Detection handles common conventions: `test_*.c`, `*_test.go`, `*.test.ts`, `*_spec.rb`

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (2-3 focused sessions)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Disabled by default. Advisory mode first, strict mode opt-in.
- **Rollback:** Revert commit. Guardrails revert to previous behavior.
- **Blast radius:** None in advisory mode. Strict mode blocks writes but is opt-in.

## Test Plan

- [ ] Unit tests: test file mapping for C, Go, TypeScript, Python, Ruby conventions
- [ ] Unit tests: write ordering detection — test-first ok, impl-first flagged
- [ ] Unit tests: strict mode blocking in PreToolUse
- [ ] Integration tests: write impl file → advisory appears in context
- [ ] Manual verification: enable TDD mode, write impl first, observe warning

## Operational Impact

- **Metrics:** `tdd_violations`, `tdd_blocks` (strict mode)
- **Logging:** `aimee: tdd: src/foo.c written without test (expected: src/tests/test_foo.c)`
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible. Write tracking is in-memory, per-session.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Test file mapping | P1 | S | High — foundation for enforcement |
| Write ordering tracking | P1 | S | High — core detection |
| Advisory mode | P1 | S | High — low-friction adoption |
| Strict mode | P2 | S | Medium — opt-in discipline |
| Delegate prompt injection | P2 | S | Medium — extends to delegates |

## Trade-offs

**Why advisory by default instead of strict?**
Strict TDD isn't always appropriate (e.g., prototyping, refactoring, config changes). Advisory surfaces the pattern without blocking productivity. Users who want strict enforcement opt in.

**Why session-scoped tracking instead of persistent?**
TDD ordering is a within-session concern. A test written in a previous session doesn't tell you whether the current implementation was test-driven. Session scope matches the actual workflow.

**Why heuristic mapping instead of configuration?**
Most projects follow standard conventions. Auto-detection works for the common case. Users with unusual conventions can set mappings via config.
