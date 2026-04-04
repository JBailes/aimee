# Worktree Enforcement Improvements

## Problem

Claude sessions sometimes work directly in `/root/aimee` instead of using the session worktree paths. Three gaps exist:

1. **The aimee repo itself has no worktree** — the workspace config points to `/root/aimee/aimee` (the binary), not `/root/aimee` (the git repo). So no worktree is created for the main repo.
2. **The guardrail hook only blocks writes to configured workspace paths** — since `/root/aimee` isn't a workspace, writes to it pass through unchecked.
3. **The session-start output is informational, not directive** — it lists paths but doesn't explicitly warn against using the main repo.

## Changes

### 1. Add aimee repo to workspaces (config fix)

Replace the `/root/aimee/aimee` workspace entry with `/root/aimee` so the main repo gets a worktree.

```bash
aimee workspace remove /root/aimee/aimee
aimee workspace add /root/aimee
```

**Risk:** The workspace name derived from the path will be `aimee`. The `create_worktree` function uses `strrchr(path, '/')` to get the name, so this gives us `aimee` — which is correct.

### 2. Strengthen session-start output (cmd_hooks.c)

Change the Working Directories output to be more directive:

```
# Working Directories
CRITICAL: This session uses isolated git worktrees. You MUST use ONLY these paths for ALL file reads, edits, and git operations. DO NOT use the original repository paths (e.g. /root/aimee/*).
- aimee: /root/.config/aimee/worktrees/{sid}/aimee
- aicli: /root/.config/aimee/worktrees/{sid}/aicli
...
```

**File:** `src/cmd_hooks.c` lines 854-856

### 3. Guardrail already enforces worktree paths (verify)

The `pre_tool_check()` in `guardrails.c:477-526` already blocks writes to workspace paths when worktrees exist. Once change #1 adds `/root/aimee` as a workspace, the guardrail will automatically protect it. No code change needed here.

## Testing

- Run `aimee session-start` and verify the aimee worktree appears
- Attempt an edit to `/root/aimee/src/foo.c` and verify the hook blocks it
- Verify the session-start output includes the stronger warning text
