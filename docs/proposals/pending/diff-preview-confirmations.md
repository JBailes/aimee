# Proposal: Diff Preview for Tool Confirmations

## Problem

When aimee's delegates or the webchat agent execute file-editing tools, the user sees either a raw tool call description or nothing at all until after the edit is applied. There is no visual preview of what will change before the edit executes.

ayder-cli implements a diff preview system in its confirmation modals:
- Before any write/replace/insert/delete operation, the TUI generates a unified diff showing additions (green) and removals (red)
- The user sees the diff in a scrollable modal and can approve, deny, or provide alternative instructions
- The `_generate_diff()` method in `tui/app.py` produces diffs for all four edit operations

This matters for aimee because:
- The webchat interface (`webchat.c`) executes tool calls via SSE without any confirmation UI — the user sees tool results after the fact
- The CLI chat (`cmd_chat.c`) auto-executes tools with no preview
- Delegates execute tools autonomously with no human-in-the-loop for destructive operations
- The guardrails system (`guardrails.c`) blocks sensitive files but doesn't preview safe edits

Evidence:
- `webchat.c` processes tool calls in `wc_execute_tool_calls()` without confirmation
- `cmd_chat.c` auto-executes all tool calls
- `agent_tools.c` has checkpoint/restore for rollback but no preview
- No diff generation exists anywhere in the codebase

## Goals

- File-editing tool calls show a unified diff preview before execution, in both webchat and CLI chat modes.
- Users can approve, reject, or modify the proposed edit.
- The diff preview works for all edit operations: write (new file / overwrite), replace (string substitution), insert (line insertion), and delete (line removal).
- Delegates running in autonomous mode skip the preview (existing behavior preserved).
- The webchat UI renders diffs with syntax-highlighted additions/removals.

## Approach

### 1. Diff generation library

Add a diff generation function to `src/agent_tools.c` or a new `src/diff.c`:

```c
typedef struct {
    char *unified_diff;  // Unified diff string
    int additions;       // Number of added lines
    int deletions;       // Number of removed lines
} diff_preview_t;

// Generate a diff preview for a proposed edit.
// Returns 0 on success, -1 on error. Caller frees preview->unified_diff.
int diff_generate_preview(const char *file_path,
                          const char *operation,  // "write", "replace", "insert", "delete"
                          const char *content,
                          const char *old_string,
                          const char *new_string,
                          int line_number,
                          diff_preview_t *preview);
```

### 2. CLI chat confirmation

In `cmd_chat.c`, before executing write/replace/insert/delete operations:
1. Generate diff preview
2. Print colorized diff to terminal (red for removals, green for additions)
3. Prompt user: `[A]pprove / [D]eny / [E]dit instructions? `
4. If approved, execute; if denied, return denial as tool result; if edit, accept new instructions

### 3. Webchat confirmation

In `webchat.c`, add a confirmation protocol via SSE:
1. Before executing a file edit, send an SSE event `tool_confirm` with the diff preview
2. The browser renders the diff in a modal with Approve/Deny buttons
3. The browser sends a POST to `/api/confirm` with the decision
4. The server thread blocks on the decision (with timeout) then proceeds

```javascript
// SSE event: tool_confirm
{
  "tool_call_id": "tc_123",
  "tool_name": "write_file",
  "file_path": "src/main.c",
  "diff": "--- a/src/main.c\n+++ b/src/main.c\n@@ -10,3 +10,5 @@...",
  "additions": 5,
  "deletions": 2
}
```

### 4. Permission-based skip

Add a `-w` (auto-approve writes) flag similar to ayder's permission model. When writes are pre-approved, skip the confirmation flow. This preserves the current autonomous behavior for delegates and adds opt-in confirmation for interactive use.

### Changes

| File | Change |
|------|--------|
| `src/diff.c` (new) | Unified diff generation for all edit operations |
| `src/headers/diff.h` (new) | Diff preview API |
| `src/cmd_chat.c` | Add confirmation prompt before file edits in interactive mode |
| `src/webchat.c` | Add `tool_confirm` SSE event and `/api/confirm` endpoint |
| `src/webchat_assets.c` | Add diff modal UI component to embedded HTML/JS |
| `src/agent_tools.c` | Call diff_generate_preview before exec_tool_write/replace |

## Acceptance Criteria

- [ ] `diff_generate_preview()` produces correct unified diffs for write, replace, insert, and delete operations
- [ ] CLI chat shows colorized diff and prompts for approval before file edits
- [ ] Webchat shows diff modal with approve/deny buttons before file edits
- [ ] Autonomous delegates skip confirmation (no behavior change)
- [ ] A `-w` flag or config option auto-approves all writes
- [ ] Diff preview works for new file creation (shows all additions)

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** M (3-4 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Confirmation is opt-in for CLI chat (disabled by default to preserve current behavior). Webchat gets it by default since it's interactive.
- **Rollback:** Remove confirmation calls; tool execution reverts to immediate.
- **Blast radius:** Worst case: confirmation modal blocks and times out. Mitigation: 30-second timeout with auto-deny.

## Test Plan

- [ ] Unit tests: diff generation for write (new file, overwrite), replace (single, multi-match), insert, delete
- [ ] Integration tests: CLI chat confirmation flow (approve, deny, timeout)
- [ ] Integration tests: webchat SSE confirm event + POST response
- [ ] Failure injection: diff generation on binary file, very large file, permission-denied file
- [ ] Manual verification: webchat diff modal renders additions in green, removals in red

## Operational Impact

- **Metrics:** `tool_confirms_total{decision="approve|deny|timeout"}`
- **Logging:** Confirmation events at DEBUG
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible — diff generation is bounded by file size

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Diff generation library | P2 | S | High — enables everything |
| Webchat confirmation modal | P2 | M | High — most visible improvement |
| CLI chat confirmation | P3 | S | Medium — CLI users are often autonomous |
| Auto-approve flag | P3 | S | Low — convenience |

## Trade-offs

- **Why not just use git diff after the fact?** Post-hoc diffs require the edit to already be applied. Preview diffs let users reject bad edits before they happen, avoiding the need for checkpoint/restore.
- **Why not make confirmation mandatory?** Delegates need to run autonomously. Making confirmation opt-in preserves the current autonomous workflow while giving interactive users control.
- **Why unified diff format?** It's universally understood, works in both terminal and web rendering, and matches git diff output that developers are already familiar with.

## Source Reference

Implementation reference: ayder-cli `tui/app.py` — `_generate_diff()` method and `CLIConfirmScreen` in `tui/screens.py` with diff rendering.
