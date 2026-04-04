# Proposal: Coordinated Parallel Execution with Shared Task State

## Problem

Aimee's delegates operate independently: each delegation is a standalone request-response. When multiple delegates work on related subtasks of a larger goal, there is no shared task list, no state coordination, and no way for one delegate's output to inform another's work.

This means:
- The primary agent must serialize and coordinate all multi-delegate workflows manually
- Delegates can produce conflicting changes (e.g., two delegates editing the same file)
- There is no way to track overall progress of a multi-delegate job
- The primary agent spends tokens on coordination that should be infrastructure

oh-my-codex's `$team` mode solves this with tmux-based parallel workers sharing a task list. While aimee shouldn't copy the tmux approach (aimee already has delegate infrastructure), the core idea — shared task state with claim-safe coordination — would make multi-delegate workflows reliable.

Evidence:
- `aimee delegate` is fire-and-forget per task
- `--background` delegates return a task ID but there's no way to group related background tasks
- No mechanism to prevent two delegates from working on the same file simultaneously
- The plan IR tracks steps but doesn't coordinate their parallel execution across delegates

## Goals

- Multiple delegates can work on subtasks of a single plan in parallel with shared state
- Tasks use claim-based locking to prevent conflicts
- The primary agent can monitor overall progress without polling each delegate individually
- File-level locking prevents concurrent edits to the same file by different delegates
- The system builds on existing plan IR and delegation, not a separate runtime

## Approach

### 1. Job model — grouping related delegations

Add a `jobs` table to group related plan steps into a coordinated execution unit:

```sql
CREATE TABLE IF NOT EXISTS jobs (
    id INTEGER PRIMARY KEY,
    plan_id INTEGER NOT NULL REFERENCES execution_plans(id),
    status TEXT DEFAULT 'pending',  -- pending, running, complete, failed, cancelled
    max_concurrent INTEGER DEFAULT 3,
    claimed_files TEXT DEFAULT '[]',  -- JSON array of file paths currently locked
    created_at TEXT DEFAULT (datetime('now')),
    updated_at TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS job_tasks (
    id INTEGER PRIMARY KEY,
    job_id INTEGER NOT NULL REFERENCES jobs(id),
    step_id INTEGER REFERENCES plan_steps(id),
    status TEXT DEFAULT 'pending',  -- pending, claimed, running, done, failed
    claimed_by TEXT,  -- delegate name
    claimed_at TEXT,
    files TEXT DEFAULT '[]',  -- JSON array of files this task touches
    result TEXT,
    error TEXT,
    created_at TEXT DEFAULT (datetime('now'))
);
```

### 2. Claim-safe task dispatch

```c
int job_create(app_ctx_t *ctx, int plan_id, int max_concurrent);
int job_claim_next(app_ctx_t *ctx, int job_id, const char *delegate_name);
int job_complete_task(app_ctx_t *ctx, int task_id, const char *result);
int job_fail_task(app_ctx_t *ctx, int task_id, const char *error);
int job_release_claim(app_ctx_t *ctx, int task_id);
```

`job_claim_next` uses a SQL transaction to atomically:
1. Find the first pending task whose `files` don't overlap with any currently-claimed task's files
2. Set it to `claimed` with the delegate name
3. Return the task details

This prevents two delegates from claiming tasks that touch the same files.

### 3. Parallel dispatch loop

```c
int job_execute(app_ctx_t *ctx, int job_id);
```

The execution loop:
1. While there are pending or running tasks:
   a. Count running tasks
   b. If running < max_concurrent and pending tasks exist with no file conflicts:
      - Claim the next task
      - Dispatch to a delegate (using existing routing, with `--background`)
   c. Poll background tasks for completion (using existing task tracking)
   d. Update job_tasks with results
2. When all tasks are done or failed → update job status

### 4. File conflict detection

Before dispatching, analyze each plan step to determine which files it will touch. This is done by:
- Parsing the step's `action` field for file paths
- Using the blast radius tool for steps that modify a function/module
- Storing the file list in `job_tasks.files`

The claim logic then ensures no two in-flight tasks share files.

### 5. CLI and MCP

```bash
aimee job start <plan_id>             # create and start a coordinated job
aimee job start <plan_id> --parallel 5  # up to 5 concurrent delegates
aimee job status <job_id>             # show task progress
aimee job cancel <job_id>             # cancel remaining tasks
```

MCP:
```json
{
  "name": "job_start",
  "description": "Start coordinated parallel execution of a plan",
  "parameters": {
    "plan_id": "integer",
    "max_concurrent": "integer (default 3)"
  }
}
```

### Changes

| File | Change |
|------|--------|
| `src/agent_jobs.c` | Implement job model: create, claim, dispatch, complete, cancel |
| `src/headers/agent_jobs.h` | Job types and function declarations |
| `src/db.c` | Add `jobs` and `job_tasks` table migrations |
| `src/mcp_tools.c` | Add `job_start`, `job_status` MCP tools |
| `src/cmd_work.c` | Add `job` subcommand |
| `src/tests/test_jobs.c` | Tests for claim logic, file conflict detection, parallel dispatch |

## Acceptance Criteria

- [ ] `aimee job start <plan_id>` creates a job and dispatches plan steps to delegates in parallel
- [ ] Tasks with overlapping files are serialized, not parallelized
- [ ] `aimee job status <id>` shows per-task progress
- [ ] Claim logic is atomic — no two delegates claim the same task
- [ ] Max concurrent limit is respected
- [ ] Failed tasks are reported without blocking other independent tasks
- [ ] Job state persists in DB across sessions

## Owner and Effort

- **Owner:** aimee
- **Effort:** L (5-6 focused sessions)
- **Dependencies:** Existing plan IR, delegation infrastructure

## Rollout and Rollback

- **Rollout:** New tables, new commands. Existing delegation unaffected.
- **Rollback:** Revert commit. Drop tables. No impact on existing plans or delegates.
- **Blast radius:** None — entirely additive.

## Test Plan

- [ ] Unit tests: claim logic — concurrent claims, file conflict detection, atomic claim
- [ ] Unit tests: max_concurrent enforcement
- [ ] Unit tests: job completion — all pass, some fail, all fail
- [ ] Integration tests: end-to-end plan → job → parallel delegation → completion
- [ ] Failure injection: delegate fails mid-task → task marked failed, other tasks continue
- [ ] Manual verification: create a 5-step plan, observe 3 running in parallel with status tracking

## Operational Impact

- **Metrics:** `jobs_started`, `jobs_completed`, `job_tasks_dispatched`, `job_file_conflicts_avoided`
- **Logging:** Per-task: `aimee: job #N task 3/5 claimed by local-codex, files: [src/foo.c]`
- **Alerts:** None
- **Disk/CPU/Memory:** N delegate calls (up to max_concurrent simultaneously). Job state ~500 bytes per task.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Job model + claim logic | P1 | M | High — core coordination |
| File conflict detection | P1 | S | High — prevents merge conflicts |
| Parallel dispatch loop | P1 | M | High — the actual parallelism |
| CLI interface | P2 | S | Medium |
| MCP tools | P2 | S | Medium |

## Trade-offs

**Why DB-based coordination instead of tmux-based (like oh-my-codex)?**
Aimee's delegates are process-based, not tmux-pane-based. DB coordination works with the existing delegate model, doesn't require tmux, and persists state naturally. tmux coordination is fragile (pane management, signal delivery, file-based mailboxes).

**Why file-level locking instead of function-level?**
File-level is simple, reliable, and sufficient. Function-level locking would require AST analysis and is overkill for the common case. If two tasks need different functions in the same file, they can be serialized safely.

**Why max_concurrent default of 3?**
Balances parallelism against resource usage. Most delegate providers have rate limits. 3 is conservative; users can increase for high-throughput setups.
