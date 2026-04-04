# Proposal: Structured Diff Output and Edit Preview

## Problem

Two pending proposals describe the same diff subsystem from different angles:

- produce structured diffs after file operations
- preview diffs before confirming interactive edits

These should be one proposal. The same diff engine should power both post-edit output and optional pre-execution previews.

## Goals

- File operations return structured diffs instead of opaque success strings.
- Interactive surfaces can preview those diffs before execution.
- CLI and webchat can render diffs consistently.
- Hooks and follow-up logic can inspect structured changes without re-reading files.

## Approach

Build one diff subsystem with two surfaces:

1. post-operation structured diff output
2. optional interactive preview/confirmation

### Structured Diff Format

Return a payload with:

- path
- hunks
- summary
- truncation indicators for large diffs

### Preview Flow

For interactive CLI/webchat sessions:

- generate the proposed diff before applying the edit
- show approve/deny flow when confirmations are enabled
- skip preview in autonomous/delegate mode

### Changes

| File | Change |
|------|--------|
| `src/diff_output.c` or `src/diff.c` | Diff computation, hunk generation, summary formatting |
| `src/headers/diff_output.h` | Public diff API |
| `src/mcp_tools.c` | Return structured diff in Edit/Write results |
| `src/render.c` | CLI diff rendering |
| `src/cmd_hooks.c` | Pass structured diff to post-tool hooks |
| `src/cmd_chat.c` | Interactive preview/confirmation flow |
| `src/webchat.c` | Preview/confirmation events and diff viewer support |
| `src/webchat_assets.c` | Diff viewer/modal UI |

## Acceptance Criteria

- [ ] Edit/Write operations return structured diff output with hunks and summary.
- [ ] Interactive sessions can show a preview diff before execution when enabled.
- [ ] Autonomous delegates skip confirmation and preserve current behavior.
- [ ] CLI and webchat render additions/removals clearly.
- [ ] Hooks can consume structured diff payloads without re-reading files.
- [ ] Large diffs are truncated safely with explicit indicators.

## Owner and Effort

- **Owner:** aimee
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Ship structured post-edit diff output first, then interactive preview as an optional layer.
- **Rollback:** Revert to plain success strings and no preview.
- **Blast radius:** Mostly display and interaction behavior around file edits.

## Test Plan

- [ ] Unit tests: diff computation, hunk generation, summary formatting
- [ ] Unit tests: new file, overwrite, replace, insert, and delete scenarios
- [ ] Integration tests: structured diff in tool results
- [ ] Integration tests: interactive approve/deny preview flow

## Operational Impact

- **Metrics:** `diff_previews_total`, `tool_confirms_total`, `diff_payload_truncations_total`
- **Logging:** DEBUG for preview and diff-generation events
- **Alerts:** None
- **Disk/CPU/Memory:** Small diff-generation overhead

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Structured post-edit diff output | P1 | S | High |
| Interactive preview flow | P2 | M | Medium |
| Hook payload enrichment | P2 | S | Medium |

## Trade-offs

- **Why merge preview and output?** They should share one diff engine and one result format.
- **Why keep preview optional?** Delegates and unattended flows must stay autonomous.
- **Why structured diff instead of plain text only?** Structured payloads support rendering, hooks, and downstream automation.
