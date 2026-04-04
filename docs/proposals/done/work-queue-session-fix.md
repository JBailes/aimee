# Proposal: Remove Work Queue Session Matching for Complete/Fail

## Problem

`aimee work complete` and `aimee work fail` always return "No matching
claimed work item found for this session." This is because `session_id()`
(config.c:9) reads `CLAUDE_SESSION_ID` from the environment, falling back
to a random ID generated from `/dev/urandom`. Each `aimee` CLI invocation
is a separate process, so without the env var set, claim and complete get
different random session IDs and never match.

The `--id` flag on `work_finish()` (cmd_work.c:327) still requires
`claimed_by = ?` to match the current session ID, so even explicit ID
targeting fails.

Because aimee is designed for a **single user, single machine, multi session**
environment, strictly locking work items to specific ephemeral session IDs
adds unnecessary friction without providing security or isolation value.

## Goals

- `aimee work complete` and `aimee work fail` work reliably without
  requiring session ID matching.
- Remove unnecessary multi-tenant lock complexity from a single-user tool.

## Approach

### 1. Remove session checks entirely

Remove the `claimed_by = ?` clause from all SQL paths in `work_finish()`,
both with and without `--id`.

```c
if (specific_id)
{
   sql = "UPDATE work_queue SET status = ?, completed_at = ?, result = ? "
         "WHERE id = ? AND status = 'claimed' "
         "RETURNING id, title";
   /* bind 1=status, 2=ts, 3=result, 4=id -- no session check */
}
else
{
   sql = "UPDATE work_queue SET status = ?1, completed_at = ?2, result = ?3 "
         "WHERE id = ("
         "  SELECT id FROM work_queue WHERE status = 'claimed' "
         "  ORDER BY claimed_at DESC LIMIT 1"
         ") "
         "RETURNING id, title";
   /* complete the most recently claimed item globally */
}
```

### 2. Accept item ID as positional argument

`aimee work claim` outputs the item ID, but `complete` requires `--id` to
use it. Accept the ID positionally for ergonomic use:

```
aimee work complete cb7909205f9c03ff
```

In `work_finish()`, treat the first positional argument as the item ID if
`--id` is not provided.

### Changes

| File | Change |
|------|--------|
| `src/cmd_work.c` | Remove `claimed_by` checks from all paths in `work_finish()`, accept positional ID |

## Acceptance Criteria

- [ ] `aimee work claim` then `aimee work complete` succeeds regardless of session
- [ ] `aimee work complete --id ITEM_ID` works regardless of session
- [ ] `aimee work complete ITEM_ID` (positional) works the same as `--id`
- [ ] `aimee work fail ITEM_ID` works regardless of session

## Owner and Effort

- **Owner:** TBD
- **Effort:** S
- **Dependencies:** None

## Priority

P0. The work queue is unusable without this fix.

## Trade-offs

Removing the session check means any session can complete any claimed item.
This is exactly what we want for a local single-user tool where the user
may have multiple terminals or agents running concurrently but they all
belong to the same user.

