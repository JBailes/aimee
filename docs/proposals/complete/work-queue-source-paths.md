# Proposal: Work Queue Source Field Should Use Full Paths

## Problem

The `source` field in work queue items uses short references like
`proposal:execution-trace-mining.md`. This is ambiguous: the file could be
in `docs/proposals/pending/`, `docs/proposals/accepted/`, or any other
subdirectory. When an agent claims a work item, it must search for the
proposal file rather than reading it directly.

During a queue processing session, every claimed item required a `find`
command to locate the actual proposal file because the source field did not
include the directory path.

## Goals

- The `source` field contains enough information to locate the file without
  searching.
- Existing short-form source references still work (backward compatible).

## Approach

### 1. Use relative paths in source field

When `add-batch --from-proposals` creates items, use the relative path from
the repository root:

```
proposal:docs/proposals/pending/execution-trace-mining.md
```

Instead of:

```
proposal:execution-trace-mining.md
```

### 2. Backward-compatible resolution

When reading the source field, try the full path first. If it does not
exist, fall back to searching `docs/proposals/*/` for the filename.

### Changes

| File | Change |
|------|--------|
| `src/cmd_work.c` | Use full relative path in `work_add_batch()` source field |

## Acceptance Criteria

- [ ] New items from `add-batch` have full relative paths in source field
- [ ] Agents can read the proposal file directly from the source field
- [ ] Old items with short-form source still work via fallback search

## Owner and Effort

- **Owner:** TBD
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change. New items get full paths.
- **Rollback:** git revert. Items get short names again.
- **Blast radius:** None. Additive change to source format.

## Test Plan

- [ ] Unit test: add-batch creates items with full paths
- [ ] Manual: claim item, verify source path is directly readable

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** None.

## Priority

P3. Minor UX improvement that saves a file search per work item.

## Trade-offs

Full paths are longer and slightly less readable in `work list` output.
The short filename is still useful for display; the full path is for
resolution. Consider showing the short name in list output but storing
the full path internally.
