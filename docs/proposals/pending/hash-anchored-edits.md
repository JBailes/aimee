# Proposal: Hash-Anchored Edit Validation

## Problem

When multiple agents or delegates work on files, edits can target stale content. Agent A reads a file, Agent B edits it, then Agent A edits based on its outdated read. The Edit tool may succeed (if the old_string still matches) but produce incorrect results, or fail with confusing errors. There is no mechanism to detect that the file changed between read and edit.

Evidence: oh-my-openagent's "hashline" system (`src/tools/hashline-edit/`) tags every line with a content hash on read (`11#VK| function hello() {`). Edits reference these hashes; if the file changed, the hash won't match and the edit is rejected. They report improved edit success rates from 6.7% to 68.3%.

## Goals

- Detect when a file has changed between Read and Edit
- Reject edits targeting stale content with a clear error
- Per-line granularity: only reject if the edited lines actually changed
- Transparent to agents — hashes are embedded in Read output

## Approach

When the MCP Read tool returns file content, compute a per-line content hash and store it server-side keyed by (session, file, line_number). When the MCP Edit tool receives an edit, verify that the hash for each affected line still matches the current file content. If not, reject with "file changed since last read — re-read before editing."

### Hash computation

```c
// xxHash32 of trimmed line content, mapped to 2-char dictionary tag
uint32_t hash = xxhash32(trimmed_line, len, 0);
const char *tag = HASH_DICT[hash % 256];
// Output: "11#VK| function hello() {"
```

### Changes

| File | Change |
|------|--------|
| `src/mcp_tools.c` | Add hash computation on Read, hash validation on Edit |
| `src/server_session.c` | Add per-session file hash cache |
| `src/headers/mcp_tools.h` | Hash dictionary, hash cache structures |

## Acceptance Criteria

- [ ] Read output includes per-line hash tags
- [ ] Edit to unmodified file succeeds normally
- [ ] Edit to file modified since last Read is rejected with clear error
- [ ] Hash cache is scoped per session and cleaned up on session end
- [ ] Performance: hash computation adds <1ms for files under 10K lines

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2–3 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; opt-in via config flag initially
- **Rollback:** Disable flag; Read/Edit revert to non-hashed behavior
- **Blast radius:** Affects Read output format (agents see hash tags) and Edit validation

## Test Plan

- [ ] Unit test: hash computation is deterministic and whitespace-normalized
- [ ] Unit test: unmodified file passes validation
- [ ] Unit test: modified line fails validation
- [ ] Unit test: modification to unrelated lines passes validation for edited lines
- [ ] Integration test: concurrent edit scenario with two sessions
- [ ] Performance test: hash 10K-line file in <1ms

## Operational Impact

- **Metrics:** Hash validation pass/fail counts per session
- **Logging:** Log validation failures at info level
- **Disk/CPU/Memory:** ~16 bytes per line per session in hash cache; negligible CPU for xxHash32

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Hash-Anchored Edits | P1 | M | High — eliminates stale-edit class of bugs |

## Trade-offs

Alternative: file-level checksums (hash entire file, not per-line). Simpler but too coarse — any change anywhere in the file would reject all edits, even to unrelated sections. Per-line hashing allows concurrent edits to different regions.

Alternative: optimistic locking via mtime. Race-prone and doesn't catch in-process rewrites from the same session.

Inspiration: oh-my-openagent `src/tools/hashline-edit/`
