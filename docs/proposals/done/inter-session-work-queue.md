# Proposal: Inter-Session Work Queue

## Problem

Aimee supports multiple concurrent sessions (each with isolated worktrees, state, and branches), but sessions cannot coordinate. Each session operates independently with no mechanism to:

1. **Distribute work across sessions.** If a user has 4 sessions running and wants each to pick up a different proposal to implement, they must manually assign work to each session. There is no shared queue or task routing.

2. **Prevent duplicate work.** Two sessions could independently decide to work on the same proposal or the same file, discovering the conflict only at PR time.

3. **Track cross-session progress.** There is no unified view of "session A is implementing proposal X, session B is implementing proposal Y, both are in progress."

The session-isolation proposal (implemented) explicitly deferred cross-session coordination. This proposal adds the minimal coordination layer needed for the "fan-out work to N sessions" pattern.

## Goals

- A shared work queue in SQLite that any session can post to and claim from.
- Atomic claim semantics so two sessions never claim the same item.
- A CLI command (`aimee work`) for posting, claiming, listing, and completing work items.
- A system-prompt addition so the AI agent knows how to interact with the queue.
- Minimal complexity: no daemon, no IPC, no polling loop. Sessions interact with the queue via explicit commands.

## Non-Goals

- Real-time notifications between sessions (no pub/sub or event bus).
- Automatic work assignment (sessions must explicitly claim).
- Cross-session dependency graphs (item A must finish before item B starts).
- Session-to-session messaging or RPC.

## Approach

### 1. Database schema

Add a `work_queue` table in a new migration:

```sql
CREATE TABLE IF NOT EXISTS work_queue (
    id TEXT PRIMARY KEY,
    title TEXT NOT NULL,
    description TEXT,          /* detailed instructions or context */
    source TEXT,               /* where this came from: "proposal:foo.md", "issue:#42", free text */
    priority INTEGER DEFAULT 0,/* higher = more important */
    status TEXT NOT NULL DEFAULT 'pending',  /* pending, claimed, done, failed, cancelled */
    claimed_by TEXT,           /* session_id of the session that claimed it */
    claimed_at TEXT,
    completed_at TEXT,
    result TEXT,               /* outcome summary, PR URL, error message */
    created_by TEXT,           /* session_id that created the item */
    created_at TEXT NOT NULL,
    metadata TEXT              /* JSON blob for extensibility */
);

CREATE INDEX IF NOT EXISTS idx_work_queue_status ON work_queue(status);
CREATE INDEX IF NOT EXISTS idx_work_queue_claimed ON work_queue(claimed_by);
```

Status transitions: `pending -> claimed -> done|failed` and `pending -> cancelled`.

### 2. Atomic claim

The critical operation is claiming a work item without races. SQLite's serialized writes make this straightforward:

```sql
UPDATE work_queue
SET status = 'claimed', claimed_by = ?, claimed_at = ?
WHERE id = (
    SELECT id FROM work_queue
    WHERE status = 'pending'
    ORDER BY priority DESC, created_at ASC
    LIMIT 1
)
AND status = 'pending'
RETURNING *;
```

The `AND status = 'pending'` in the outer UPDATE prevents TOCTOU races. If two sessions execute this concurrently, SQLite's write serialization ensures one succeeds and the other gets zero rows (and retries to claim the next item). The `RETURNING *` gives the claimer the full item without a second query.

A session can also claim a specific item by ID:

```sql
UPDATE work_queue
SET status = 'claimed', claimed_by = ?, claimed_at = ?
WHERE id = ? AND status = 'pending'
RETURNING *;
```

### 3. CLI interface

New subcommand: `aimee work <action>`:

| Command | Description |
|---------|-------------|
| `aimee work add "title" [--desc "..."] [--source "..."] [--priority N]` | Post a work item |
| `aimee work add-batch --from-proposals [--dir path]` | Scan pending proposals and add one item per proposal |
| `aimee work claim [--id ITEM_ID]` | Claim the next (or specific) pending item |
| `aimee work complete [--id ITEM_ID] [--result "..."]` | Mark claimed item as done |
| `aimee work fail [--id ITEM_ID] [--result "..."]` | Mark claimed item as failed |
| `aimee work list [--status pending\|claimed\|done\|all]` | List items, default: pending + claimed |
| `aimee work cancel --id ITEM_ID` | Cancel a pending item |
| `aimee work release --id ITEM_ID` | Release a claimed item back to pending (e.g., session crashed) |
| `aimee work clear --status done` | Remove completed items |

Note: `aimee queue` already exists for running multiple delegation tasks in parallel within a single session. `aimee work` is distinct: it coordinates work items *across* sessions.

Example workflow for the "implement proposals" use case:

```bash
# User (or one session) populates the queue:
aimee work add-batch --from-proposals

# Each of 4 sessions claims and works:
aimee work claim
# -> Returns: { id: "abc", title: "delegation-robustness", source: "proposal:delegation-robustness.md", ... }

# Session reads the proposal, implements it, creates PR
# Then marks done:
aimee work complete --result "PR #42 created"
```

### 4. add-batch from proposals

The `add-batch --from-proposals` command scans `docs/proposals/pending/` and creates one queue item per `.md` file, using the filename as the title and `proposal:<filename>` as the source. It skips proposals that already have a pending or claimed queue item with the same source (idempotent).

```c
int cmd_queue_add_batch_proposals(sqlite3 *db, const char *proposals_dir)
{
    /* Default: docs/proposals/pending/ relative to workspace root */
    DIR *d = opendir(proposals_dir);
    struct dirent *ent;
    int added = 0;

    while ((ent = readdir(d)) != NULL) {
        if (!ends_with(ent->d_name, ".md")) continue;

        char source[256];
        snprintf(source, sizeof(source), "proposal:%s", ent->d_name);

        /* Skip if already queued */
        if (queue_item_exists_by_source(db, source)) continue;

        /* Extract title from first H1 line */
        char title[256];
        extract_proposal_title(proposals_dir, ent->d_name, title, sizeof(title));

        queue_add(db, title, NULL, source, 0);
        added++;
    }
    closedir(d);
    return added;
}
```

### 5. System prompt integration

Add a `# Work Queue` section to the system prompt (in `cmd_chat.c` and `webchat.c`):

```
# Work Queue

You can coordinate with other aimee sessions via a shared work queue.

- `aimee work claim` to pick up the next available work item
- `aimee work complete --result "summary"` when done
- `aimee work fail --result "reason"` if you cannot complete it
- `aimee work list` to see all items and their status

When you claim an item, read its description/source to understand what to do.
If the source is a proposal (e.g., "proposal:foo.md"), read the proposal file,
implement it, and create a PR. Then mark the item complete with the PR URL.
```

### 6. Session-start integration

On `session-start`, if there are pending queue items, include a summary in the context output:

```
# Work Queue
There are 7 pending items in the work queue. Run `aimee work claim` to pick one up.
Currently claimed by other sessions: 3 items.
```

This is informational only. The session does not auto-claim; the user or agent decides.

### 7. Stale claim recovery

If a session crashes without completing or failing its claimed items, those items are stuck in `claimed` status. Two recovery mechanisms:

1. **Manual release:** `aimee queue release --id ITEM_ID` sets status back to `pending`.
2. **Auto-release on session cleanup:** When the worktree/session janitor detects a stale session (no activity for 24h, session state file missing), it releases any items claimed by that session:
   ```sql
   UPDATE work_queue SET status = 'pending', claimed_by = NULL, claimed_at = NULL
   WHERE claimed_by = ? AND status = 'claimed';
   ```

## Acceptance Criteria

- [ ] `work_queue` table created via migration, survives upgrade/downgrade.
- [ ] `aimee queue add "title"` creates an item visible in `aimee queue list`.
- [ ] Two concurrent `aimee queue claim` calls never return the same item.
- [ ] `aimee queue add-batch --from-proposals` creates items for all pending proposals, is idempotent.
- [ ] `aimee queue complete` and `aimee queue fail` update status correctly.
- [ ] System prompt mentions the queue when items exist.
- [ ] Session-start context includes queue summary when items are pending.
- [ ] `aimee queue release` returns a claimed item to pending.
- [ ] Stale session cleanup releases orphaned claims.

## Files Changed

| File | Change |
|------|--------|
| `src/db.c` | New migration: `work_queue` table and indexes |
| `src/cmd_work.c` (new) | CLI handlers for `aimee work` subcommands |
| `src/cmd_chat.c` | Add work queue section to system prompt |
| `src/webchat.c` | Add work queue section to system prompt |
| `src/cmd_hooks.c` | Queue summary in session-start context output |
| `src/cmd_table.c` | Register `work` subcommand |
| `src/Makefile` | Add `cmd_work.o` to build |
| `src/cli_client.c` | RPC routes for `work.*` methods |
| `src/server.c` | Server dispatch for `work.*` methods |
| `src/server_state.c` | Server-side `work.*` handlers |
| `src/headers/server.h` | Handler declarations |

## Test Plan

- Unit test: create item, claim it, verify second claim gets different item or NULL.
- Unit test: `add-batch` with mock proposals directory, verify idempotency.
- Integration test: two concurrent shell processes both run `aimee queue claim`, verify no overlap.
- Integration test: claim an item, kill the session, run stale cleanup, verify item returns to pending.

## Rollback Plan

Drop the `work_queue` table and remove the migration. The queue is purely additive; no existing functionality depends on it. System prompt and session-start changes are guarded by "if queue items exist" checks, so they become no-ops.

## Operational Impact

- **Database:** One new table, lightweight (text rows, no blobs). No measurable performance impact.
- **Disk:** Negligible. Queue items are small text records.
- **Concurrency:** SQLite WAL handles concurrent claims. The atomic UPDATE pattern avoids explicit locking.
- **Backward compatibility:** Fully backward compatible. Sessions that do not use the queue are unaffected.

## Trade-offs

**Pro:**
- Simple pull-based model: no daemon, no IPC, no polling. Sessions claim when ready.
- Atomic claim via SQLite eliminates race conditions without distributed locks.
- `add-batch --from-proposals` makes the "implement all proposals" use case a two-command workflow.
- Fully optional: sessions that do not interact with the queue work exactly as before.
- Leverages existing shared SQLite database; no new infrastructure.

**Con:**
- Pull-based only: a session must explicitly run `aimee queue claim`. There is no push notification when new items are added.
- No dependency ordering: items are independent. If proposal B depends on proposal A being merged first, the user must handle sequencing manually (or use priority to influence order).
- Queue items are free-form text. There is no schema validation on what constitutes a valid work item.
- The system prompt addition is a soft directive. The AI agent may not always check the queue unprompted.

## Future Extensions (Not In Scope)

- **Push notifications:** A lightweight file-based signal (e.g., inotify on a sentinel file) so sessions learn about new queue items without polling.
- **Dependency chains:** `depends_on` column so item B blocks until item A completes.
- **Auto-claim on session-start:** Session automatically claims an item if the queue is non-empty and no other work is assigned.
- **Cross-repo work items:** Queue items that target specific sub-projects, with automatic worktree setup for that project.
