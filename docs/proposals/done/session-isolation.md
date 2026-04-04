# Proposal: Session Isolation for Concurrent Aimee Instances

## Problem

Aimee currently assumes a single active session. All state lives in shared locations:

1. **`~/.config/aimee/session.state`** (single JSON file, last-write-wins). Contains session mode (plan/implement), guardrail mode, active task ID, and seen paths. Two concurrent sessions clobber each other: session A switches to plan mode, session B writes implement mode over it, session A's guardrails silently stop working.

2. **`~/.config/aimee/aimee.db`** (single SQLite database). WAL mode allows concurrent reads, and writers queue via busy timeout. The database itself won't corrupt, but semantic conflicts arise: both sessions create tasks, both modify the same memory entries, wrapup from session A promotes/compacts memories that session B is still using.

3. **`~/.config/aimee/config.json`** (shared config). Read-mostly, but `aimee setup` writes to it. If session A runs setup while session B is loading config, session B gets a partial read.

4. **Git working trees.** The sub-project repositories (wol, acktng, web-tng, etc.) are shared directories on disk. Two sessions editing the same files clobber each other's uncommitted changes. Both share the same git index (staging area), so `git add` in one session stages files the other didn't intend to commit. Both are on the same branch, so commits from one session change the other's baseline mid-work.

The result: two SSH sessions running aimee-backed Claude instances will interfere with each other unpredictably. Plan mode leaks across sessions, task ownership is ambiguous, wrapup can discard another session's in-flight L0 memories, and concurrent edits to the same repository corrupt each other's work.

## Goals

- Two (or more) independent SSH sessions can each run an aimee-backed Claude instance concurrently without interference.
- Each session has its own mode, guardrail state, active task, and seen-path tracking.
- Each session gets its own isolated git working tree, branch, and staging area for every sub-project it touches.
- Shared resources (memory, index, rules) remain shared, since that is the point of persistent knowledge.
- Minimal complexity. No daemon, no IPC, no distributed locking.

## Approach: Per-Session State File + Session-Scoped DB Rows

### 1. Session identity

Each aimee invocation within a Claude session shares a single session ID. Claude Code already provides this via the `CLAUDE_SESSION_ID` environment variable (a UUID set per conversation). Aimee reads it at startup. If absent (e.g., standalone CLI usage), generate a random UUID and export it for child processes.

```c
const char *session_id(void)
{
   static char id[64];
   if (id[0]) return id;

   const char *env = getenv("CLAUDE_SESSION_ID");
   if (env && env[0]) {
      snprintf(id, sizeof(id), "%s", env);
      return id;
   }

   /* Fallback: generate from /dev/urandom */
   unsigned char buf[16];
   FILE *f = fopen("/dev/urandom", "r");
   if (f) { fread(buf, 1, 16, f); fclose(f); }
   snprintf(id, sizeof(id),
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
            buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
   return id;
}
```

### 2. Per-session state file

Replace the single `session.state` with `session-<ID>.state`:

```
~/.config/aimee/session-abc123.state
~/.config/aimee/session-def456.state
```

Changes to `config_output_dir()` are not needed. Instead, the state path construction in `cmd_hooks.c:57-58` changes from:

```c
snprintf(state_path, sizeof(state_path), "%s/session.state", config_output_dir());
```

to:

```c
snprintf(state_path, sizeof(state_path), "%s/session-%s.state", config_output_dir(), session_id());
```

The same change applies in `cmd_core.c` (mode/plan/implement commands) and anywhere else `session.state` is referenced.

**Cleanup:** `aimee wrapup` deletes its own session state file when done. A `session-start` can also prune stale session files older than 24 hours (simple `stat()` + `unlink()` loop).

### 3. Session-scoped database operations

The database is shared, which is correct for persistent knowledge (L1-L3 memories, index, rules, anti-patterns). But certain tables need session awareness:

| Table | Current behavior | Change needed |
|-------|-----------------|---------------|
| `windows` | Has `session_id` column | Already scoped. No change. |
| `decisions` | Has window FK | Already scoped via window. No change. |
| `memories` | Has `source_session` column | Already tracks origin. No change needed for reads (all sessions should see all memories). Wrapup promotion should only promote L0 memories from *its own* session. |
| `memory_provenance` | Has `session_id` column | Already scoped. No change. |
| `tasks` | No session column | Add `session_id TEXT` column. Tasks created in one session should not be made "active" in another unless explicitly adopted. |
| `agent_jobs` | Has `lease_owner` | Use session ID as lease owner. Already designed for this. |
| `agent_log` | No session scoping | Add `session_id TEXT` column for attribution. Low priority. |
| `context_cache` | Session context hash cache | Add `session_id TEXT` column. Cache is only valid within the session that created it. |

### 4. Wrapup isolation

The `wrapup` command currently:
1. Folds L0 memories into L1
2. Promotes based on use count / confidence
3. Compacts duplicates
4. Learns from conversation

With session isolation, wrapup must:
- Only fold/promote L0 memories where `source_session = current_session_id`
- Compaction and deduplication are global (safe, since they merge content)
- Delete its own session state file on completion

### 5. Task ownership

Add `session_id` to the `tasks` table. When a session sets `active_task_id`, it only affects that session's state file, not a global value. Task listing without a session filter shows all tasks (useful for cross-session awareness). Task state changes (done, failed) from any session are fine since they represent real progress.

### 6. Config file safety

`config.json` writes are rare (only `aimee setup`). Use atomic write (write to temp file, then `rename()`) to prevent partial reads. This is a small change in `config_save()`:

```c
int config_save(const config_t *cfg)
{
   /* ... build JSON string ... */
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, getpid());
   FILE *fp = fopen(tmp, "w");
   /* ... write ... */
   fclose(fp);
   rename(tmp, path);  /* atomic on same filesystem */
}
```

### 7. Git worktree isolation

This is the core mechanism for preventing two sessions from clobbering each other's file edits, git staging area, and branch state.

#### How git worktrees work

`git worktree add <path> -b <branch>` creates a new working directory backed by the same `.git` object store. Each worktree has its own HEAD, index (staging area), and checked-out files. Two worktrees cannot have the same branch checked out simultaneously (git enforces this). Worktrees are cheap: they share all objects, packfiles, and refs with the parent repo.

#### Design

**Every session** gets its own git worktree for every configured workspace that is a git repository. No session ever operates on the real working tree directly. This eliminates the "which session is primary?" question and makes behavior uniform regardless of how many sessions are active.

#### Worktree lifecycle

**Creation (session-start):**

1. For each configured workspace that is a git repo, create a worktree:
   ```
   git -C <workspace-root> fetch origin
   git -C <workspace-root> worktree add \
     ~/.config/aimee/worktrees/<session-id>/<project-name> \
     -b aimee/session/<session-id-short> \
     origin/main
   ```
   The worktree starts from `origin/main` (freshly fetched) so every session begins from the latest upstream state.
2. Record the worktree mapping in the session state file:
   ```json
   {
     "session_mode": "implement",
     "worktrees": {
       "wol": "/root/.config/aimee/worktrees/abc123/wol",
       "wol-realm": "/root/.config/aimee/worktrees/abc123/wol-realm"
     }
   }
   ```
3. Inject the worktree paths into the session-start context output so Claude knows to use them:
   ```
   # Working Directories
   This session is using isolated git worktrees. Use these paths for all file operations:
   - wol: /root/.config/aimee/worktrees/abc123/wol
   - wol-realm: /root/.config/aimee/worktrees/abc123/wol-realm
   All file reads, edits, and git operations for these projects MUST use the worktree paths.
   ```

**During the session:**

- The pre-hook (`aimee hooks pre`) enforces worktree usage for all write operations. If Claude attempts to write to a path inside a configured workspace's real working tree (e.g., `/root/aicli/wol/src/Foo.cs`), the pre-hook **blocks the operation** (exit code 2) with a message directing Claude to use the worktree path instead. This is a hard guardrail, not a suggestion. Writes to paths outside configured workspaces (e.g., `/tmp`, `~/.config`) are unaffected.
- The post-hook (`aimee hooks post`) re-indexes using the worktree path, so the index reflects the session's version of the files.
- Git operations (commit, push, branch) happen naturally in the worktree since Claude is operating from that directory.
- Bash commands that write to real workspace paths (detected via `is_write_command()` + path analysis) are also blocked by the pre-hook.

**Cleanup (wrapup):**

1. For each worktree in the session state:
   - Check if the worktree branch has unpushed commits. If yes, warn (do not delete).
   - If the branch was pushed or has no commits, remove the worktree:
     ```
     git -C <workspace-root> worktree remove <worktree-path>
     ```
   - Delete the session branch if it was merged or has no unique commits:
     ```
     git -C <workspace-root> branch -d aimee/session/<session-id-short>
     ```
2. Remove the worktree directory under `~/.config/aimee/worktrees/<session-id>/`.

**Stale cleanup (session-start):**

On every session-start, scan `~/.config/aimee/worktrees/` for directories whose corresponding session state file is missing or older than 24 hours. For each:
- Run `git worktree remove` (with `--force` if needed).
- Delete the session branch if safe.
- Remove the directory.

#### What the user sees

**Every session** gets worktrees. The `session-start` context output lists the worktree paths for each project. Claude operates from those paths for the duration. Each session has its own branch, its own staging area, and its own files. When done, each session creates its own PR from its own branch. The real working trees under `~/aicli/` are never modified by Claude sessions.

#### Edge cases

**Both sessions edit the same file in the same project.** Each has its own copy (different worktrees). The conflict surfaces naturally when both create PRs. This is the same workflow as two developers working on the same file in different branches, which is the intended model.

**Workspace not a git repo (e.g., config-only directory).** Skip worktree creation for that workspace. The session state isolation (sections 1-6) still applies.

**Session crashes without wrapup.** The worktree persists on disk. The next session-start prunes it (24h stale check). The branch remains but is harmless.

**User wants to inspect a session's work.** The worktree is a normal directory on disk. The user can `cd` to the worktree path and browse, diff, or run tests directly. The path is printed in the session-start output.

## Affected Files

| File | Change |
|------|--------|
| `config.c` | Add `session_id()` function. Atomic config writes. |
| `guardrails.h` | Add `worktrees` field to `session_state_t` (mapping of project name to worktree path). |
| `guardrails.c` | Hard block in `pre_tool_check()`: if a write targets a real workspace path and the session has a worktree for that project, block with exit code 2 and direct Claude to the worktree path. |
| `cmd_hooks.c` | Use `session_id()` in state path construction (lines 57-58). Worktree creation logic in `cmd_session_start()`. Worktree path injection into session context output. Post-hook re-index using worktree paths. |
| `cmd_hooks.c` (wrapup) | Scope L0 promotion to current session. Delete session state file. Remove worktrees (with unpushed-commit safety check). Prune stale worktrees. |
| `cmd_core.c` | Use `session_id()` in mode/plan/implement commands for state path. |
| `db.c` | Migration 24: add `session_id` to `tasks`, `agent_log`, `context_cache`. |
| `tasks.c` | Pass session ID when creating tasks. Filter active task by session. |
| `memory.c` | Wrapup promotion: filter `source_session` for L0 fold. |
| `agent_jobs.c` | Use `session_id()` as lease owner (may already work if lease_owner is set by caller). |

## What Does NOT Change

- **Database file:** Still a single `aimee.db`. SQLite WAL handles concurrent access.
- **Index:** Shared. Two sessions indexing the same file is idempotent (keyed by mtime). Sessions with worktrees index their own copy.
- **Rules:** Shared. Read-only during sessions.
- **L1-L3 memories:** Shared. Both sessions benefit from accumulated knowledge.
- **Config structure:** Same `config.json`, just written atomically.
- **Hook registration:** Same hooks in `~/.claude/settings.json`. Each Claude session invokes aimee with its own `CLAUDE_SESSION_ID`.
- **CLI interface:** No new flags required. Session ID is derived from environment automatically.
- **Real working trees:** Never modified by Claude sessions. The user's manual work in those directories is undisturbed.

## Trade-offs

**Pro:**
- Zero user-facing complexity. No flags, no configuration, no daemon.
- Leverages existing `CLAUDE_SESSION_ID` from Claude Code.
- Shared knowledge (memories, index) is preserved across sessions.
- SQLite WAL already handles the hard part of concurrent DB access.
- Git worktrees give full file/branch/staging isolation using a native git feature with minimal disk cost (shared object store).
- Uniform behavior: every session works the same way regardless of how many are active.
- The real working trees are never modified by Claude sessions, so the user's manual work in those directories is never disturbed.
- Each session starts from a fresh `origin/main`, so there is no stale-branch problem.
- Concurrent edits surface as normal branch conflicts at PR time, which is the standard multi-developer workflow.

**Con:**
- Stale session files and worktrees accumulate if wrapup is not called (mitigated by 24h pruning on session-start).
- Claude must use worktree paths instead of the natural sub-project paths. This relies on session-start context injection, which is a soft directive (Claude could ignore it or get confused). The pre-hook path remapping provides a safety net but cannot fully enforce it.
- Worktree branches (`aimee/session/<id>`) add noise to the branch list. Cleanup on wrapup mitigates this.
- A long-running session A won't see tasks created by session B unless it queries explicitly (acceptable: sessions are independent by design).
- Git worktrees require that each worktree be on a different branch. This is fine since each session gets its own `aimee/session/<id>` branch automatically.

## Migration Path

1. Add `session_id()` to `config.c` (or a new `session.c`).
2. Add migration 24 for new columns.
3. Update state path construction in `cmd_hooks.c` and `cmd_core.c`.
4. Add worktree creation logic to `cmd_session_start()`.
5. Add worktree path injection to `build_session_context()`.
6. Add worktree path remapping to `pre_tool_check()`.
7. Scope wrapup promotion by session.
8. Add worktree cleanup to wrapup and stale session pruning.
9. Atomic config writes.
10. Update tests to cover concurrent session scenarios.

## Not In Scope

- **Cross-session coordination** (e.g., "session A is editing file X, warn session B"). This could be added later using a lightweight lock table in SQLite, but adds complexity without clear immediate value.
- **Session listing/management CLI** (e.g., `aimee sessions list`). Nice-to-have, not required for isolation.
- **Per-session databases.** This would fully isolate but would lose the shared-knowledge benefit that makes aimee useful. Rejected.
