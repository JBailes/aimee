# Proposal: Work Queue Claim Skip, Filtering, and Priority Ordering

## Problem

`aimee work claim` always returns the first pending item sorted by priority
DESC, created_at ASC. There is no way to skip items. If the first item is
too complex, unsuitable for the current session, or depends on other work,
the only workaround is: claim it, release it, claim again (which returns
the same item).

A second, related problem: the queue mixes items of different effort sizes
(S/M/L) and types (feature, refactoring, bugfix). During a real queue
processing session, an agent had to claim and abandon 4 consecutive
file-splitting refactors before finding a tractable feature item. There is
no way to filter by effort or type without reading each proposal.

Additionally, items that are released and return to pending may not retain
their original queue position relative to newly added items. The priority
field exists in the schema but is not well exposed in `add` or `add-batch`.

## Goals

- Agents can skip items that are not suitable for their current session.
- Agents can filter by effort size or item type without reading proposals.
- Queue ordering is predictable and respects explicit priority.

## Approach

### 1. Skip flag on claim

Add `--skip N` to `aimee work claim` to skip the first N pending items:

```sql
SELECT id FROM work_queue WHERE status = 'pending'
ORDER BY priority DESC, created_at ASC
LIMIT 1 OFFSET ?
```

### 2. Metadata columns for filtering

Add `effort` and `tags` columns to `work_queue`:

```sql
ALTER TABLE work_queue ADD COLUMN effort TEXT DEFAULT '';
ALTER TABLE work_queue ADD COLUMN tags TEXT DEFAULT '';
```

`effort` holds S/M/L extracted from the proposal's `## Owner and Effort`
section. `tags` holds comma-separated labels (e.g. "feature,refactor,ci").

### 3. Filter flags on claim

```
aimee work claim --effort S        # only claim small items
aimee work claim --tag feature     # only claim feature items
aimee work claim --exclude-tag refactor  # skip refactoring items
```

SQL:
```sql
SELECT id FROM work_queue WHERE status = 'pending'
AND (? IS NULL OR effort = ?)
AND (? IS NULL OR tags LIKE '%' || ? || '%')
ORDER BY priority DESC, created_at ASC
LIMIT 1 OFFSET ?
```

### 4. Auto-extract metadata in add-batch

`add-batch --from-proposals` parses each proposal file:
- Extract effort from `**Effort:** S` pattern
- Extract tags from section headers or a `## Tags` section if present
- Set priority from `## Priority` section (P0=30, P1=20, P2=10, P3=0)

### 5. Claim by ID

`aimee work claim --id ITEM_ID` already works. Document this in help text.

### 6. Priority in add/add-batch

Ensure `aimee work add --priority N` and `add-batch` surface priority
clearly. Default priority should be documented (currently 0).

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Add migration: `effort` and `tags` columns on work_queue |
| `src/cmd_work.c` | Add `--skip`, `--effort`, `--tag`, `--exclude-tag` flags to `work_claim()`; auto-extract metadata in `work_add_batch()` |

## Acceptance Criteria

- [ ] `aimee work claim --skip 1` skips the first pending item
- [ ] `aimee work claim --skip N` with N >= pending count returns "no items"
- [ ] `aimee work claim --effort S` only returns items with effort "S"
- [ ] `aimee work claim --tag feature` only returns items tagged "feature"
- [ ] `aimee work claim --exclude-tag refactor` skips items tagged "refactor"
- [ ] `add-batch --from-proposals` auto-extracts effort, tags, and priority
- [ ] `aimee work list` displays effort and tags columns
- [ ] Help text documents `--id`, `--skip`, `--effort`, and `--tag` flags
- [ ] Priority ordering is consistent after release/re-add

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** DB migration adds columns. Existing items have empty effort/tags until re-imported.
- **Rollback:** git revert. Empty columns are harmless.
- **Blast radius:** None. Additive columns and flags.

## Test Plan

- [ ] Unit test: --skip 0 behaves like default
- [ ] Unit test: --skip 1 returns second pending item
- [ ] Unit test: --skip beyond count returns no items
- [ ] Unit test: --effort S filters correctly
- [ ] Unit test: --tag and --exclude-tag filter correctly
- [ ] Unit test: add-batch extracts effort from proposal file
- [ ] Manual: agent filters for small features, skips refactoring

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** Negligible.

## Priority

P2. Quality-of-life improvement for agents working through queues.

## Trade-offs

Metadata extraction from proposals is heuristic (regex on markdown). If a
proposal does not follow the standard format, effort and tags will be empty.
This is acceptable because empty metadata just means no filtering, not
incorrect filtering. Manual `--effort` and `--tag` flags on `work add` serve
as the authoritative override.
