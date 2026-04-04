# Proposal: Structural Worktree Isolation

## Problem

Worktree enforcement currently relies on guardrail hooks to block reads and writes
to real workspace paths. This is fragile:

1. **Hook gaps**: The PreToolUse matcher must list every tool that touches files.
   Missing `Read|Glob|Grep` (fixed in PR #99) left reads unblocked for months.
2. **Implicit cwd**: Git commands operate on the current working directory. The
   guardrail must parse commands and check cwd separately — easy to miss edge
   cases (`git add` was missing until PR #99).
3. **Race at startup**: The agent starts with cwd set to the real workspace.
   Session-start output tells it to use worktree paths, but this is a soft
   directive the agent can miss (e.g., if output is truncated).
4. **Hook-dependent**: Agents without hook support (or with different hook schemas)
   get no worktree enforcement at all.

The root cause: the agent process starts in the wrong directory and we try to
redirect it after the fact. The right fix is to start it in the right directory.

## Goals

- Agent processes start with cwd set to a worktree, not the real workspace.
- Worktree isolation is structural (cwd-based), not enforcement-based (hook-based).
- Guardrail hooks become a safety net, not the primary mechanism.
- No change to the user's workflow — `aimee` (no args) still launches a session.
- Works for all providers (Claude, Codex, Gemini, built-in chat).

## Approach

### Current flow (main.c:161-191)

```
aimee (no args)
  → cmd_session_start()    # creates worktrees, prints context to stdout
  → execlp("claude")       # launches provider with cwd = user's cwd
```

### Proposed flow

```
aimee (no args)
  → cmd_session_start()    # creates worktrees, saves state
  → load session state     # read back worktree paths
  → chdir(primary_worktree)  # set cwd to the project worktree
  → execlp("claude")       # provider starts in the worktree
```

### Determining the primary worktree

When the user runs `aimee` from inside a workspace (e.g., `/root/aimee/src`),
match their cwd against configured workspaces to find which worktree to use:

```c
session_state_t state;
session_state_load(&state, state_path);

/* Find which workspace contains the user's cwd */
char cwd[MAX_PATH_LEN];
getcwd(cwd, sizeof(cwd));
const char *wt = worktree_for_path(&state, &cfg, cwd);

if (wt) {
    /* Compute equivalent path inside the worktree.
     * E.g., cwd=/root/aimee/src, workspace=/root/aimee, worktree=/tmp/wt/aimee
     * → chdir to /tmp/wt/aimee/src */
    const char *suffix = cwd + strlen(matching_workspace);
    char target[MAX_PATH_LEN];
    snprintf(target, sizeof(target), "%s%s", wt, suffix);
    chdir(target);
}
```

If the user is not inside any workspace (e.g., running from `~`), use the first
worktree as a default, or skip the chdir and rely on hooks as today.

### Changes

| File | Change |
|------|--------|
| `src/main.c` | After `cmd_session_start()`, load session state, resolve primary worktree from cwd, `chdir()` before `execlp()`. |

Note: `cli_main.c` does not need this change — it forwards to the server via
`forward_command("session-start")` and does not exec a provider. Only `main.c`
(direct/monolith mode) execs the provider and needs the chdir.

### What does NOT change

- Hook registration and guardrail enforcement remain as-is (safety net).
- `session-start` output still prints worktree paths (belt and suspenders).
- Worktree creation logic is unchanged.
- `aimee session-start` (subcommand form, used by hooks) is unchanged.
- Users who launch `claude` directly (without `aimee`) still get hook-based enforcement.

## Acceptance Criteria

- [ ] `aimee` (no args) from `/root/aimee/src` launches Claude with cwd in the
      aimee worktree's `src/` subdirectory.
- [ ] `aimee` from `/root/aimee` launches with cwd at the aimee worktree root.
- [ ] `aimee` from `~` (outside any workspace) launches successfully (no crash,
      falls back to current behavior).
- [ ] `aimee` with provider=codex/gemini also gets worktree cwd.
- [ ] Built-in chat (`use_builtin_cli=true`) also gets worktree cwd.
- [ ] Agent cannot read or write to real workspace paths from the worktree cwd
      without explicit path traversal.

## Owner and Effort

- **Owner:** TBD
- **Effort:** S (the core change is ~20 lines in main.c)
- **Dependencies:** PR #99 (guardrail enforcement gaps) should land first so the
  safety-net layer is complete.

## Rollout and Rollback

- **Rollout:** Ships with the next `aimee` binary build. Takes effect immediately
  for users who launch sessions via `aimee` (no args).
- **Rollback:** Revert the commit. The chdir is the only behavioral change;
  removing it restores the previous flow exactly.
- **Blast radius:** Only affects sessions launched via `aimee` (no args). Users
  who run `claude` directly are unaffected (they still get hook-based enforcement).

## Test Plan

- [ ] Unit test: `worktree_for_path` with cwd inside a nested subdirectory of a
      workspace returns the correct worktree path.
- [ ] Integration test: launch `aimee` from a workspace subdirectory, verify the
      agent's initial cwd (via `pwd` in first Bash call) is inside the worktree.
- [ ] Integration test: launch `aimee` from outside any workspace, verify it
      launches without error.
- [ ] Manual: run a full session via `aimee`, create a branch, commit, push —
      all operations stay within the worktree.

## Operational Impact

- **Metrics:** None (no new counters).
- **Logging:** `aimee` will log (stderr) when it changes cwd to a worktree:
  `aimee: session cwd: /root/.config/aimee/worktrees/{sid}/aimee/src`
- **Alerts:** None.
- **Disk/CPU/Memory:** No change (worktrees are already created).

## Trade-offs

**Why not remove guardrail enforcement entirely?**
Users who launch `claude` (or other agents) directly — without going through
`aimee` — still need hook-based enforcement. The hooks also catch path arguments
that reference real workspace paths even when cwd is correct (e.g.,
`Read /root/aimee/src/foo.c` from inside a worktree). Both layers serve a purpose.

**Why not make this a separate `aimee launch` command?**
`aimee` with no args already does exactly this: session-start then exec provider.
Adding a separate command creates two ways to start a session, which is confusing.
The no-args path is the right place for this.

**Alternative: set `--cwd` flag on the provider exec?**
Claude Code and Codex don't have a `--cwd` flag. `chdir()` before `execlp()` is
the standard Unix mechanism and works for any provider.
