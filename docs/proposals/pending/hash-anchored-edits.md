# Proposal: File Operation Safety Guards

## Problem

Two pending proposals cover closely related file-safety failures:

- stale edits based on outdated reads
- blind overwrites of existing files without reading first

These belong together. Both protect against agents writing to files based on stale or nonexistent knowledge of current file state.

## Goals

- Block overwriting existing files that have not been read in the current session.
- Detect when a file changed between read and edit.
- Give agents clear recovery guidance instead of ambiguous failures.
- Keep protections scoped per session so legitimate workflows still work.

## Approach

Build one file-operation safety layer with two checks:

1. read-before-write guard for existing files
2. hash-anchored stale-edit validation for edited regions

### Read-Before-Write

Track files read in the current session. If `Write` targets an existing unread file, block it with guidance to use `Read`/`Edit` first.

### Hash-Anchored Edit Validation

On `Read`, compute per-line hashes and cache them per session. On `Edit`, verify the targeted lines still match the last-read hashes; if not, reject with a stale-read error.

### Changes

| File | Change |
|------|--------|
| `src/mcp_tools.c` | Read tracking, per-line hashing, and stale-edit validation |
| `src/server_session.c` | Per-session read-set and line-hash cache |
| `src/headers/mcp_tools.h` | Hash/cache structures and helpers |

## Acceptance Criteria

- [ ] Writing an existing unread file is blocked with guidance.
- [ ] Writing a new file still succeeds normally.
- [ ] Editing unchanged lines after a read succeeds normally.
- [ ] Editing lines that changed since the last read fails with a clear stale-read message.
- [ ] Session cleanup frees read-tracking and hash state.

## Owner and Effort

- **Owner:** aimee
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Enable read-before-write guard first, then add hash-anchored edit validation behind a config flag if needed.
- **Rollback:** Disable stale-edit validation and fall back to read-before-write only.
- **Blast radius:** File-editing tools only.

## Test Plan

- [ ] Unit tests: write-before-read blocking and allow cases
- [ ] Unit tests: deterministic line hashing and stale-edit detection
- [ ] Integration tests: concurrent edit scenarios across sessions

## Operational Impact

- **Metrics:** `blocked_blind_writes_total`, `stale_edit_rejections_total`
- **Logging:** WARN on blocked writes and stale-edit failures
- **Alerts:** None
- **Disk/CPU/Memory:** Small per-session caches

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Read-before-write guard | P1 | S | High |
| Hash-anchored stale-edit validation | P1 | M | High |

## Trade-offs

- **Why merge these proposals?** They both enforce the same invariant: file modifications must be based on current observed file state.
- **Why not rely on mtimes or full-file hashes?** They are too coarse and reject safe concurrent edits in unrelated regions.
- **Why keep read-before-write even with line hashes?** Hash validation only helps when a read already happened; blind overwrites still need a hard guard.
