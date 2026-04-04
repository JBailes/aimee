# Proposal: Fix Worktree Enforcement Bypass

## Problem

The worktree enforcement guardrail in `pre_tool_check()` failed to block Edit and Bash tool calls that wrote to `/root/dev/aimee/src/*` (the real repo) instead of the worktree at `/root/.config/aimee/worktrees/4e3dc7e4-1ef1-aa5f-3ebb-ff797bc293ef/aimee/src/*`.

Evidence:
- Session state (`session-4e3dc7e4-*.state`) shows aimee worktree registered with `created=1` and `workspace_root=/root/dev/aimee`
- The agent made 20+ Edit calls to `/root/dev/aimee/src/*.c` during a work queue processing session
- None were blocked by the PreToolUse hook
- The agent also used Bash with `cd /root/dev/aimee && git commit` which bypasses path-based enforcement

## Goals

- Edit/Write tool calls to real workspace paths are reliably blocked when a worktree exists
- Bash tool calls with `cd <real-workspace>` patterns are detected and blocked
- The guardrail failure mode is debuggable (log when enforcement fires or doesn't)

## Investigation Areas

1. **Hook invocation reliability** — Does Claude Code's PreToolUse hook fire for every Edit call? Check if the hook matcher regex `"Edit|Write|MultiEdit|Bash|Read|Glob|Grep"` correctly matches.

2. **Session state loading** — The hook runs `aimee hooks pre` as a subprocess. Does it load the correct session state file? The session ID comes from `CLAUDE_SESSION_ID` env var — verify this is propagated to the hook subprocess.

3. **Bash `cd` chains** — `cd /root/dev/aimee && git commit` doesn't have the workspace path as a git argument. The `is_write_command` + `extract_paths_shlex` approach won't catch it. The cwd check (`worktree_for_path(state, &wcfg, cwd)`) only checks the cwd of the hook process, not the cd target in the command.

4. **`/root/dev` parent workspace** — `/root/dev` is also in the workspace list. The "most specific match" logic should pick `/root/dev/aimee` over `/root/dev`, but verify this works correctly.

## Approach

### 1. Add `cd` target detection in Bash commands

When `is_write_command` matches a Bash command, also extract `cd` targets:
```c
// Extract cd target from "cd /path && ..." patterns
if (strstr(cmd, "cd ")) {
   // parse the path after "cd "
   // check if it falls in a workspace with a worktree
}
```

### 2. Add diagnostic logging

When the worktree enforcement check runs (or doesn't), log to stderr:
```
aimee: worktree check: tool=Edit path=/root/dev/aimee/src/memory.c ws=aimee -> BLOCKED
aimee: worktree check: tool=Edit path=/root/.config/aimee/worktrees/.../src/memory.c -> ALLOWED
```

### 3. Verify hook invocation

Add a counter or timestamp to the session state that increments on each `pre_tool_check` call. If the count doesn't increase after Edit calls, the hook isn't firing.

## Changes

| File | Change |
|------|--------|
| `src/guardrails.c` | Add cd-target extraction for Bash write commands |
| `src/guardrails.c` | Add diagnostic stderr logging for worktree enforcement |
| `src/cmd_hooks.c` | Add hook invocation counter to session state |

## Acceptance Criteria

- [ ] Edit calls to `/root/dev/aimee/src/memory.c` are blocked when aimee worktree exists
- [ ] `cd /root/dev/aimee && git commit` is blocked when aimee worktree exists
- [ ] Diagnostic logs show enforcement decisions

## Owner and Effort

- **Owner:** aimee core
- **Effort:** M
- **Priority:** P1 — worktree isolation is a core safety feature

## Test Plan

- [ ] Unit test: Edit to real workspace path with worktree → blocked
- [ ] Unit test: Bash `cd /real/path && git commit` with worktree → blocked
- [ ] Manual: start session, verify Edit to real repo is blocked, Edit to worktree is allowed
