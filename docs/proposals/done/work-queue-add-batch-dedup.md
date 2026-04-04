# Proposal: Work Queue add-batch Deduplication Against Done and Rejected Proposals

## Problem

`aimee work add-batch --from-proposals` still only deduplicates against queue
rows in `pending` or `claimed`. It does not account for the actual proposal
workflow now used in this repo:

- implemented proposals move to `docs/proposals/done/`
- rejected proposals move to `docs/proposals/rejected/`

That means rerunning `add-batch` can recreate work items for proposals that are
no longer actionable.

## Goals

- Never queue a proposal that already exists in `done/` or `rejected/`
- Also skip proposals that already have a `done` queue row
- Report why an item was skipped

## Approach

Before inserting a work item for `pending/<name>.md`, check:

1. `docs/proposals/done/<name>.md`
2. `docs/proposals/rejected/<name>.md`
3. existing queue rows with `status IN ('pending', 'claimed', 'done')`

This proposal pairs naturally with `work-queue-source-paths.md`, but it does
not depend on it.

## Changes

| File | Change |
|------|--------|
| `src/cmd_work.c` | Add done/rejected filesystem checks and done-row dedup in `work_add_batch()` |

## Acceptance Criteria

- [ ] `add-batch` skips proposals present in `done/`
- [ ] `add-batch` skips proposals present in `rejected/`
- [ ] `add-batch` skips proposals with a `done` work-queue row
- [ ] Output distinguishes already-queued from already-resolved items
