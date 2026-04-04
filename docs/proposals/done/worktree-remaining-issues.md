# Worktree Remaining Issues

Follow-up from the worktree enforcement work in PR #95.

## 1. Stale worktree accumulation (821MB and growing)

The pruning logic in `prune_stale_sessions()` (`cmd_hooks.c:607`) only runs as a
background fork during `session-start` and only targets sessions >24 hours old.
In practice, 17 session worktree directories have accumulated totalling 821MB.

**Root causes:**
- `session-start` creates new worktrees but pruning happens asynchronously in a
  background child. If the child fails or is killed, stale sessions persist.
- `cmd_wrapup` is tied to the `SessionEnd` hook, but Claude Code sessions don't
  always trigger a clean shutdown (e.g. terminal close, timeout, crash).
- The orphan check (line 664-683) looks for worktree dirs without state files,
  but every session here has a state file, so they persist until 24h.

**Proposed fixes:**
- Add an `aimee session clean` command for manual cleanup of all stale sessions.
- Lower the staleness threshold from 24h to something configurable (e.g. 4h
  default) since most Claude sessions are under an hour.
- Add a max-sessions cap: if more than N session directories exist, prune the
  oldest regardless of age.

## 2. Worktree branches from main, not the working branch

`create_worktree()` (`cmd_hooks.c:509`) always branches from `main`. When a
session needs to work on an existing feature branch (e.g. `aimee/ci-workflow`),
the worktree contains main's code, not the branch's. This caused build failures
in the enforcement PR work when the worktree Makefile was from main (which
lacked the `DIRECT` target present on the feature branch).

**Proposed fix:**
- Detect the current branch of the workspace at session-start time. If the
  workspace is on a non-main branch, create the worktree from that branch's
  HEAD instead of main.
- Alternatively, allow `aimee session-start --branch <name>` to specify the
  target branch for worktree creation.

## 3. Workspace config not managed in version control

The workspace list lives in `~/.config/aimee/config.json`, a local runtime file.
The fix in PR #95 (changing `/root/aimee/aimee` to `/root/aimee`) only applies
to the current machine. A fresh setup would have the same broken entry.

**Proposed fix:**
- Source workspace definitions from the checked-in `aimee.workspace.yaml`
  manifest rather than (or in addition to) the local config file. The manifest
  is already in the repo root and lists all projects.
- `aimee setup` should validate workspace paths exist and are git repos,
  warning on entries like `/root/aimee/aimee` that point to files, not
  directories.

## 4. Worktree name collision when workspace path ends with repo name

The workspace name is derived from `strrchr(path, '/')`. For `/root/aimee`,
this gives `aimee`. If any sub-project were also named `aimee`, there'd be a
name collision. Not currently a problem but fragile.

**Proposed fix:**
- Allow explicit workspace names in the config or manifest, falling back to
  basename derivation only when no name is specified.

## Priority

1. **Stale accumulation** (high) — 821MB disk waste, growing with every session
2. **Branch mismatch** (medium) — causes build failures when working on feature branches
3. **Config not in VCS** (medium) — fresh setups will have the broken workspace entry
4. **Name collision** (low) — theoretical, no current conflict
