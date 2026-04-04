# Proposal: Session Safety — Verify Gate, Merged-PR Enforcement, and Worktree Rewrite

## Problem

Three interrelated safety issues with aimee's session management:

### 1. Pushes to merged PRs

Despite rules in CLAUDE.md and over a dozen `-50` scoring penalties, sessions continue pushing to branches of already-merged PRs. The current merged-PR check in `handle_git_push()` (mcp_git.c:499-518) shells out to `gh pr list` at push time, which is slow and only catches MCP pushes — raw `git push` via Bash bypasses it entirely.

### 2. Verify gate is bypassable

Both `handle_git_push()` and `handle_git_pr()` accept a `skip_verify` parameter that lets sessions skip verification entirely. This defeats the purpose of the verify gate. Additionally, the current verify records only the HEAD hash in `.aimee/.last-verify` — if you verify then amend a commit message (no file change), verify is needlessly invalidated. The verify should be based on file content/timestamps: if the same files exist unchanged an hour later, the previous verify remains valid.

### 3. Worktree system is overcomplicated and fragile

The current worktree implementation in `guardrails.c` (lines 690-970) is ~280 lines of complex logic that:
- Iterates all configured workspaces to find matches
- Has separate code paths for Claude Code worktrees vs aimee worktrees
- Does lazy creation on first write with fallback-on-failure
- Stores worktree entries in session state with deferred creation flags
- Creates worktrees under `~/.config/aimee/worktrees/<session-id>/<project-name>` — far from the project itself
- Has a DB registry, GC system, stale-session detection, and size-budget enforcement

What it should do: before any write operation, if the session's CWD is inside a git repo (has a `.git` root), auto-create a single worktree for that repo in a sibling directory next to the `.git` root, named after the aimee session ID. That's it.

## Goals

- Merged-PR pushes are blocked at both MCP and hooks layers with no override
- `skip_verify` is removed — verify is mandatory when configured
- Verify validity is based on file mtimes + 1-hour TTL, not HEAD hash
- Worktree creation is simplified: one worktree per git repo per session, created next to the project root, tied to `session_id()`
- The ~280 lines of worktree logic in guardrails.c is replaced with ~50 lines

## Approach

### Part A: Verify Gate Hardening

#### A1. Remove `skip_verify`

Remove the `skip_verify` parameter from both `handle_git_push()` and `handle_git_pr()` (action=create). Verification is mandatory when `.aimee/project.yaml` has verify steps. No escape hatch — if verify is broken, fix the verify steps.

#### A2. File-timestamp-based verify caching

Replace the HEAD-hash state file with a richer `.aimee/.last-verify` that records:
- Unix timestamp of when verify passed
- A hash of all tracked file paths + their mtimes

Verify check logic:
1. Load `.aimee/.last-verify` — if missing, verify required
2. If more than 3600 seconds have elapsed, verify required
3. Compute current file-mtime hash: hash of `git ls-files` paths + their `stat()` mtimes
4. If hash matches stored hash, verify is still valid
5. Otherwise, verify required

### Part B: Merged-PR Enforcement

#### B1. Block at MCP layer (push + PR create)

The existing `handle_git_push()` check stays. Add the same check to `handle_git_pr()` action=create — block creating a PR if the branch already has a merged PR.

#### B2. Block at hooks layer

Add merged-PR detection to `pre_tool_check()` in `guardrails.c` for Bash commands containing `git push`. This catches raw pushes that bypass MCP tools.

### Part C: Worktree Rewrite

#### C1. New worktree model

Replace the entire worktree system with a simple model:

- **When**: Before any write operation (Edit, Write, Bash write command), if the target path is inside a git repo
- **Where**: The worktree lives as a sibling directory next to the project root. For a project at `/root/dev/aimee` (with `/root/dev/aimee/.git`), the worktree is created at `/root/dev/aimee-<short-session-id>` (e.g., `/root/dev/aimee-fadc648f`)
- **How to find git root**:

```c
git -C <target-dir> rev-parse --show-toplevel
```
If this returns a path, that's the project root. If it fails, the path is not in a git repo — allow the write (no worktree needed).

#### C3. Enforcement

On pre-tool check for write operations:
1. Resolve target path to its git root
2. If no git root, allow (not a git repo)
3. Compute expected worktree path: `<parent-of-git-root>/<project-name>-<short-session-id>`
4. If target path is already inside a worktree path (contains session ID in path), allow
5. If worktree doesn't exist, create it and block with redirect message
6. If worktree exists, block with redirect message

#### C4. Cleanup

Worktree cleanup on session end:
- Check if worktree has uncommitted changes or unpushed commits
- If clean, remove with `git worktree remove`
- If dirty, leave in place and log a warning

#### C5. What gets deleted

The following are no longer needed:
- `worktree_entry_t` struct and `worktrees[]` array in `session_state_t`
- `worktree_ensure()`, `worktree_resolve_path()`, `worktree_for_path()`
- `worktree_db_register()`, `worktree_db_touch()`
- The entire worktree GC system (`worktree_gc_stale()`, `worktree_gc_deleted()`, `worktree_gc_budget()`)
- The DB `worktrees` table
- The `~/.config/aimee/worktrees/` directory tree

### Changes

| File | Change |
|------|--------|
| `src/mcp_git.c` | Remove `skip_verify` from push and PR create; add merged-PR check to PR create |
| `src/git_verify.c` | Replace HEAD-hash with file-mtime-hash + timestamp; rename `verify_check_head` → `verify_check` |
| `src/headers/git_verify.h` | Update function signatures |
| `src/guardrails.c` | Replace ~280 lines of worktree logic with ~50 lines of git-root + sibling-worktree logic |
| `src/guardrails.c` | Add merged-PR detection for Bash `git push` commands |
| `src/guardrails.c` | Remove `worktree_ensure()`, `worktree_resolve_path()`, `worktree_for_path()`, `worktree_db_*()`, `worktree_gc_*()` |
| `src/headers/guardrails.h` | Remove `worktree_entry_t`, simplify `session_state_t` |
| `src/db.c` | Remove worktrees table migration (or leave as no-op for backwards compat) |

## Acceptance Criteria

### Verify gate
- [ ] `skip_verify` parameter no longer exists on `git_push` or `git_pr`
- [ ] After `git_verify` passes, push succeeds within 1 hour if no tracked files changed
- [ ] Modifying any tracked file invalidates the verify regardless of time
- [ ] After 1 hour, verify is invalidated even with no file changes

### Merged-PR enforcement
- [ ] `git_push` to a branch with a merged PR returns an error
- [ ] `git_pr action=create` on a branch with a merged PR returns an error
- [ ] Bash `git push` on a merged-PR branch is blocked by hooks

### Worktree rewrite
- [ ] Write to `/root/dev/aimee/src/foo.c` is blocked and redirected to `/root/dev/aimee-<session-id>/src/foo.c`
- [ ] Worktree is auto-created on first write attempt (no manual step needed)
- [ ] Worktree is a sibling of the project root, not under `~/.config/aimee/worktrees/`
- [ ] Only the root `.git` project gets a worktree — subdirectories don't each get their own
- [ ] Session cleanup removes clean worktrees, warns on dirty ones

## Owner and Effort

- **Owner:** aimee core
- **Effort:** L (three subsystems touched, but each change is individually straightforward)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct code change, effective immediately on next build. Existing worktrees under `~/.config/aimee/worktrees/` can be cleaned up manually or by a one-time migration script.
- **Rollback:** Revert commit. Old worktree paths will no longer be auto-created but existing ones still work as git worktrees.
- **Blast radius:** All sessions. The worktree path change means any in-progress worktree work under the old path structure would need to be committed/pushed before upgrading.

## Test Plan

- [ ] Unit test: `verify_compute_file_hash()` returns same hash for unchanged files
- [ ] Unit test: `verify_compute_file_hash()` changes when file mtime changes
- [ ] Unit test: `verify_check()` valid within TTL, invalid after TTL
- [ ] Unit test: push to merged-PR branch → blocked (MCP)
- [ ] Unit test: Bash `git push` to merged-PR branch → blocked (hooks)
- [ ] Unit test: write to git repo path → blocked with sibling worktree redirect
- [ ] Unit test: write to worktree path → allowed
- [ ] Unit test: write to non-git path → allowed
- [ ] Integration test: end-to-end session with verify → push → PR create
- [ ] Manual: start session, edit file, verify worktree auto-created as sibling

## Operational Impact

- **Metrics:** None new
- **Logging:** Verify check logs invalidation reason; worktree creation/redirect logged to stderr
- **Alerts:** None
- **Disk/CPU/Memory:** `git ls-files` + stat is fast (<100ms). `git rev-parse --show-toplevel` is <10ms. Worktrees as siblings use same disk as before but are easier to find. No more DB writes for worktree tracking.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Remove skip_verify | P1 | S | Closes enforcement gap |
| File-mtime verify cache | P1 | M | Better UX, fewer false invalidations |
| Merged-PR enforcement (hooks) | P1 | S | Closes Bash bypass |
| Worktree rewrite | P1 | M | Simplifies ~300 lines to ~50, fixes path locality |

## Trade-offs

- **File-mtime hash vs git tree hash**: Git tree hash (`git write-tree`) is more accurate but requires a clean index. File mtimes are cheaper and work with dirty trees. Touching a file without changing content resets mtime and invalidates verify — acceptable since verify is fast.
- **1-hour TTL**: Arbitrary but reasonable. Could be configurable in `project.yaml` later. Not adding config now.
- **No skip_verify escape hatch**: Intentional. If verify is broken, fix the verify steps.
- **Sibling worktrees vs subdirectory worktrees**: Siblings (e.g., `aimee-fadc648f/` next to `aimee/`) are visible in the parent directory listing, which could be noisy if many sessions are active. But they're much easier to find and work with than paths buried under `~/.config/`. Git's own `git worktree list` will show them correctly.
- **No DB registry**: The filesystem _is_ the registry. `ls /root/dev/aimee-*/` shows all active worktrees. This is simpler and eliminates DB/filesystem inconsistency bugs.
- **Removing old worktree GC**: The GC was needed because worktrees were hidden under `~/.config/`. With sibling worktrees, users can see and clean them up directly. Session cleanup handles the common case.
