# Proposal: Session Safety — Configurable Verify Gate, Merged-PR Enforcement, Worktree Rewrite

## Problem

Three interrelated safety issues with aimee's session management:

### 1. Verify gate has no enforcement

`git_verify` runs steps defined in `.aimee/project.yaml` and records a HEAD hash, but neither `handle_git_push()` nor `handle_git_pr()` check verification status before proceeding. Verification is purely informational — there is no gate.

### 2. Pushes to merged PRs

Despite rules in CLAUDE.md and over a dozen `-50` scoring penalties, sessions continue pushing to branches of already-merged PRs. There is no merged-PR check in `handle_git_push()` or `handle_git_pr()`. Raw `git push` via Bash bypasses MCP entirely.

### 3. Worktree system is overcomplicated and fragile

The current worktree implementation spans `worktree.c` (~400 lines) and `guardrails.c` (~230 lines of enforcement) with:
- Workspace-config-based matching against configured directories
- Separate code paths for Claude Code worktrees vs aimee worktrees
- Lazy creation on first write with fallback-on-failure
- Worktree entries in session state with deferred creation flags
- Worktrees under `~/.config/aimee/worktrees/<session-id>/<project-name>` — far from the project
- DB registry, GC system, stale-session detection, and size-budget enforcement
- Thread-based parallel creation with gate/signal/wait primitives

What it should do: before any write operation, if the target path is inside a git repo, auto-create a single worktree as a hidden sibling directory. That's it.

## Goals

- Verify enforcement is opt-in via `project.yaml` — when enabled, push/PR create are gated
- Verify validity uses file-mtime hashing + 1-hour TTL instead of HEAD hash
- Merged-PR pushes are blocked at both MCP and hooks layers with no override
- Direct pushes to main/master are blocked
- Worktree creation is simplified: one hidden sibling worktree per git repo per session
- ~630 lines of worktree logic replaced with ~80 lines

## Approach

### Part A: Configurable Verify Gate

#### A1. New `project.yaml` schema (breaking change)

```yaml
verify:
  enforce: false          # opt-in, default false
  steps:
    - name: build
      run: cd src && make
    - name: tests
      run: cd src && make unit-tests
```

The old flat format (where `- name:` lines appear directly under `verify:`) is dropped. Projects must migrate to the nested `steps:` sub-key format.

- **`enforce`** (bool, default `false`): When `true`, `git_push` and `git_pr create` call `verify_check()` and refuse to proceed if verification is stale/failed. When `false`, verify exists but doesn't gate anything.
- **`steps`** (list): Same `name` + `run` pairs as before, just nested under `steps:`.

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

#### A3. Wire enforcement into push/PR

In `handle_git_push()` and `handle_git_pr()` (action=create), before proceeding:
```c
verify_config_t vcfg;
if (verify_load_config(NULL, &vcfg) == 0 && vcfg.enforce) {
    char vmsg[256];
    if (!verify_check(NULL, vmsg, sizeof(vmsg))) {
        return mcp_error("push blocked: %s", vmsg);
    }
}
```

### Part B: Merged-PR Enforcement

#### B1. Block at MCP layer — `handle_git_push()`

Before pushing, check if the current branch has a merged PR:
```c
char pr_cmd[512];
snprintf(pr_cmd, sizeof(pr_cmd),
    "gh pr list --head '%s' --state merged --json number --jq '.[0].number' 2>/dev/null",
    branch);
char *pr_out = run_cmd(pr_cmd, &rc);
if (pr_out && pr_out[0] && pr_out[0] != '\n') {
    return mcp_error("push blocked: branch '%s' has merged PR #%s. Create a new branch.",
                     branch, pr_out);
}
```

#### B2. Block at MCP layer — `handle_git_pr()` action=create

Same merged-PR check before running `gh pr create`.

#### B3. Block at hooks layer

Add merged-PR detection to `pre_tool_check()` in `guardrails.c` for Bash commands containing `git push`. This catches raw pushes that bypass MCP tools.

#### B4. Block direct pushes to main/master

`handle_git_push()` must refuse when the current branch is `main` or `master`. Same check in hooks layer for raw `git push`.

### Part C: Worktree Rewrite

#### C1. New worktree model

- **When**: Before any write operation (Edit, Write, Bash write command), if the target path is inside a git repo
- **Where**: Hidden dot-prefixed sibling of the project root. For `/root/dev/aimee` (with `/root/dev/aimee/.git`), the worktree is created at `/root/dev/.aimee-<short-session-id>` (e.g., `/root/dev/.aimee-fadc648f`). The dot prefix keeps worktrees hidden from normal `ls` output.
- **How**: `git -C <project-root> worktree add <sibling-path> -b aimee/session/<short-id> <base-branch>`
- **Session binding**: Worktree name and branch include the first 8 chars of `session_id()`. Only the owning session writes to it.
- **No DB registry**: The directory's existence is the registry. `ls -d /root/dev/.aimee-*/` shows all active worktrees.

#### C2. Enforcement (~50 lines in `pre_tool_check()`)

For write operations:
1. Resolve target path to its git root via `git_repo_root()`
2. If no git root → allow (not a git repo)
3. If target path already contains the session ID → allow (already in worktree)
4. Compute expected worktree: `<parent>/.<project-name>-<short-session-id>`
5. If worktree doesn't exist, create it
6. Block with redirect message

For read operations (Read/Glob/Grep):
- If a worktree exists for the target's git root → block with redirect
- If no worktree created yet → allow (non-mutating, safe)

**Critical**: Paths inside a worktree directory (containing session ID or `.claude/worktrees/`) are always allowed — never blocked.

#### C3. What gets deleted

| Component | Location |
|-----------|----------|
| `worktree_entry_t` struct | `guardrails.h` |
| `worktrees[]` array + count in `session_state_t` | `guardrails.h` |
| `worktree_thread_arg_t` struct | `guardrails.h` |
| Gate fields (`worktree_ready`, `wt_mutex`, `wt_cond`) | `guardrails.h` |
| `worktree_ensure()` | `worktree.c` |
| `worktree_resolve_path()` | `worktree.c` |
| `worktree_for_path()` | `worktree.c` |
| `worktree_for_path_if_created()` | `worktree.c` |
| `worktree_db_register()` | `worktree.c` |
| `worktree_db_touch()` | `worktree.c` |
| `worktree_gc()` | `worktree.c` |
| `worktree_gate_*()` + `worktree_thread_fn()` | `worktree.c` |
| `dir_size_bytes()` | `worktree.c` |
| Auto-provision block in `pre_tool_check()` | `guardrails.c` |
| Worktree enforcement block in `pre_tool_check()` | `guardrails.c` |
| `worktrees` DB table | `db.c` (leave migration as no-op) |

#### C4. New session state

```c
typedef struct {
    char git_root[MAX_PATH_LEN];
    char worktree_path[MAX_PATH_LEN];
    int created;  // 0=pending, 1=created, -1=failed
} sibling_worktree_t;

// In session_state_t, replace worktrees[] with:
sibling_worktree_t sibling_worktrees[MAX_WORKTREES];
int sibling_worktree_count;
```

#### C5. Cleanup on session end

- Check if worktree has uncommitted changes or unpushed commits
- If clean → `git worktree remove`
- If dirty → leave in place, log warning

## Changes

| File | Change |
|------|--------|
| `src/headers/git_verify.h` | Add `enforce` field to `verify_config_t` |
| `src/git_verify.c` | Parse nested `verify:` → `enforce:` + `steps:` format; replace HEAD-hash with file-mtime-hash + TTL; rename `verify_check_head` → `verify_check` |
| `src/mcp_git.c` | Add verify gate in push/PR create; add merged-PR check in push/PR create; block push to main/master |
| `src/headers/guardrails.h` | Replace `worktree_entry_t` with `sibling_worktree_t`; simplify `session_state_t`; remove old function declarations |
| `src/worktree.c` | Gut and rewrite: ~400 lines → ~80 lines (create sibling worktree, simple lookup, session cleanup) |
| `src/guardrails.c` | Replace ~230 lines of worktree logic in `pre_tool_check()` with ~50 lines; add merged-PR check for Bash `git push` |
| `src/db.c` | Leave migration 33 as no-op |
| `src/tests/test_guardrails.c` | Update tests for new worktree model |
| `src/tests/test_mcp_git.c` | Add tests for verify gate + merged-PR blocking |

## Acceptance Criteria

### Verify gate
- [ ] `enforce: true` in project.yaml causes push/PR create to check verification
- [ ] `enforce: false` (default) does not gate push/PR create
- [ ] Old flat `verify:` format is rejected (must use nested `steps:`)
- [ ] After `git_verify` passes, push succeeds within 1 hour if no tracked files changed
- [ ] Modifying any tracked file invalidates verify regardless of time
- [ ] After 1 hour, verify is invalidated even with no file changes

### Merged-PR enforcement
- [ ] `git_push` to a branch with a merged PR returns an error
- [ ] `git_pr action=create` on a branch with a merged PR returns an error
- [ ] Bash `git push` on a merged-PR branch is blocked by hooks
- [ ] Push to main/master is blocked at MCP and hooks layers

### Worktree rewrite
- [ ] Write to `/root/dev/aimee/src/foo.c` is blocked and redirected to `/root/dev/.aimee-<session-id>/src/foo.c`
- [ ] Write to worktree path `/root/dev/.aimee-<session-id>/src/foo.c` is allowed (not blocked)
- [ ] Worktree is auto-created on first write attempt
- [ ] Worktree is a hidden sibling of the project root (dot-prefixed)
- [ ] Only the git root gets a worktree — subdirectories don't each get their own
- [ ] Session cleanup removes clean worktrees, warns on dirty ones
- [ ] Claude Code `.claude/worktrees/` paths are never blocked

## Test Plan

- [ ] Unit test: `verify_load_config()` parses nested format with `enforce` field
- [ ] Unit test: `verify_load_config()` rejects old flat format
- [ ] Unit test: `verify_compute_file_hash()` returns same hash for unchanged files
- [ ] Unit test: `verify_compute_file_hash()` changes when file mtime changes
- [ ] Unit test: `verify_check()` valid within TTL, invalid after TTL
- [ ] Unit test: push to merged-PR branch → blocked (MCP)
- [ ] Unit test: push to main/master → blocked (MCP)
- [ ] Unit test: Bash `git push` to merged-PR branch → blocked (hooks)
- [ ] Unit test: write to git repo path → blocked with hidden sibling worktree redirect
- [ ] Unit test: write to `.aimee-*` worktree path → allowed
- [ ] Unit test: write to non-git path → allowed
- [ ] Integration test: end-to-end session with verify → push → PR create
- [ ] Manual: start session, edit file, verify worktree auto-created as hidden sibling

## Trade-offs

- **File-mtime hash vs git tree hash**: Git tree hash (`git write-tree`) is more accurate but requires a clean index. File mtimes are cheaper and work with dirty trees. Touching a file without changing content resets mtime and invalidates verify — acceptable since verify is fast.
- **1-hour TTL**: Arbitrary but reasonable. Could be configurable in `project.yaml` later.
- **Opt-in enforcement**: Verify gate defaults to off. Projects must explicitly set `enforce: true`. This avoids breaking projects that use verify informally.
- **Breaking YAML format**: Old flat `verify:` format is dropped. This forces a migration but keeps the parser simple and avoids ambiguity between old and new formats.
- **Dot-prefixed sibling worktrees**: Hidden from normal `ls` but findable with `ls -a` or `git worktree list`. Much more accessible than `~/.config/aimee/worktrees/`.
- **No DB registry**: The filesystem is the registry. `ls -da /root/dev/.aimee-*/` shows all active worktrees. Eliminates DB/filesystem inconsistency bugs.
- **No worktree GC**: Session cleanup handles the common case. Users can see and clean up dot-prefixed siblings directly.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Configurable verify gate | P1 | M | Closes enforcement gap with opt-in control |
| File-mtime verify cache | P1 | M | Better UX, fewer false invalidations |
| Merged-PR enforcement | P1 | S | Closes MCP + Bash bypass |
| Block push to main/master | P1 | S | Prevents accidental direct pushes |
| Worktree rewrite | P1 | M | Simplifies ~630 lines to ~80, fixes path locality |

## Rollout and Rollback

- **Rollout:** Direct code change, effective immediately on next build. Existing worktrees under `~/.config/aimee/worktrees/` can be cleaned up manually. Projects using the old flat `verify:` format must migrate to nested `steps:`.
- **Rollback:** Revert commit. Old worktree paths will no longer be auto-created but existing ones still work as git worktrees.
- **Blast radius:** All sessions. The worktree path change means any in-progress worktree work under the old path structure would need to be committed/pushed before upgrading. Projects with `verify:` in old format will lose verify functionality until migrated.
