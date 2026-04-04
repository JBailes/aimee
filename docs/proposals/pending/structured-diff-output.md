# Proposal: Structured Diff Output from File Operations

## Problem

When agents edit or write files through aimee, the tool result is a simple success/failure message. Agents must re-read the file to see what changed, users see "file written successfully" instead of a diff, and post-tool hooks can't inspect changes.

This impacts CLI and webchat: CLI users see no diff, webchat users miss the opportunity for an inline diff viewer.

The `soongenwong/claudecode` repo at `rust/crates/runtime/src/file_ops.rs` implements structured patch output with hunks and summaries.

## Goals

- File edit/write operations return structured diffs in tool results.
- CLI renders inline diffs with color (green additions, red deletions).
- Webchat renders diffs in a collapsible diff viewer component.
- Post-tool hooks receive structured diff for inspection.
- Agents see a change summary without re-reading the file.

## Approach

### Structured Patch Format

```json
{
  "status": "ok",
  "path": "src/config.c",
  "hunks": [
    {
      "old_start": 42, "old_lines": 3,
      "new_start": 42, "new_lines": 5,
      "lines": [
        " static int default_timeout = 30;",
        "-static int max_retries = 3;",
        "+static int max_retries = 5;",
        "+static int backoff_ms = 100;"
      ]
    }
  ],
  "summary": "+2 -1 lines in 1 hunk"
}
```

### Changes

| File | Change |
|------|--------|
| `src/diff_output.c` (new) | Diff computation, structured hunk generation, summary formatting |
| `src/headers/diff_output.h` (new) | Public API |
| `src/mcp_tools.c` | Edit/Write tool results include structured diff |
| `src/render.c` | Add `render_diff()` for CLI colored diff |
| `src/cmd_hooks.c` | Pass structured diff to post-tool hooks |
| `src/webchat.c` | Render diff viewer component for file change results |

### Webchat Integration

Webchat renders diffs as collapsible panels with red/green highlighting, file path header, and expand/collapse toggle.

## Acceptance Criteria

- [ ] Edit tool results include structured diff with hunks and summary
- [ ] Write tool results for existing files include diff; new files show full content as additions
- [ ] CLI renders diffs with color
- [ ] Webchat renders diffs in a collapsible viewer
- [ ] Post-tool hooks receive structured diff in payload
- [ ] Large diffs truncated to 50 hunks / 500 lines with indicator

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** S (1-2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Always-on — diff included in tool results. Rendering adapts to surface.
- **Rollback:** Return to plain success messages.
- **Blast radius:** Large diffs could bloat tool results. Truncation mitigates.

## Test Plan

- [ ] Unit tests: diff computation, hunk parsing, summary generation
- [ ] Integration tests: edit a file via MCP tool, verify structured diff
- [ ] Failure injection: binary file, empty edit, very large diff
- [ ] Manual verification: edit a file, verify CLI and webchat diffs

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Diff computation + output | P3 | S | Medium |
| CLI colored diff | P3 | S | Medium |
| Webchat diff viewer | P3 | S | High — review quality |
| Hook payload enrichment | P3 | S | Low |

## Source Reference

Implementation reference: `soongenwong/claudecode` at `rust/crates/runtime/src/file_ops.rs`.
