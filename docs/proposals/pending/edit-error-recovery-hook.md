# Proposal: Edit Error Recovery Hook

## Problem

AI agents frequently fail Edit tool calls due to stale assumptions about file content. Common failures: "old_string not found" (file changed since last read), "old_string found multiple times" (ambiguous match), "old_string and new_string are identical". When these errors occur, agents often retry blindly with the same wrong assumption instead of re-reading the file. This wastes tokens and causes cascading failures, especially in delegate sessions where there's no human to intervene.

Evidence: oh-my-openagent's edit-error-recovery hook (`src/hooks/edit-error-recovery/hook.ts`) addresses this exact pattern. Their implementation intercepts Edit tool output, pattern-matches known error strings, and injects a system reminder forcing the agent to re-read before retrying.

## Goals

- Agents always re-read a file after a failed Edit before attempting another edit
- Reduce wasted token loops from blind edit retries in delegate sessions
- Zero false positives — only trigger on genuine Edit tool errors

## Approach

Add a post-tool hook in the MCP tool pipeline (`mcp_tools.c`) that inspects Edit tool results. When the result contains a known error pattern, append a recovery directive to the tool output.

### Error patterns to detect

- `old_string not found in file` or similar
- `old_string matches multiple locations`
- `old_string and new_string are identical`

### Recovery directive appended

```
[EDIT ERROR — Read the file before retrying]
Your edit failed because your assumption about the file content was wrong.
1. Read the file to see its actual current state
2. Retry with corrected old_string based on what the file actually contains
Do NOT retry the same edit without reading first.
```

### Changes

| File | Change |
|------|--------|
| `src/mcp_tools.c` | Add post-execution check in edit tool handler for known error patterns |
| `src/headers/mcp_tools.h` | Add error pattern constants |

## Acceptance Criteria

- [ ] Edit tool errors matching known patterns get recovery directive appended
- [ ] Non-edit tool errors are not affected
- [ ] Recovery directive is concise (<100 words) to minimize context waste
- [ ] Works for both direct edits and delegate-initiated edits

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (< 1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion, always active
- **Rollback:** Remove the post-check; edit tool returns raw errors as before
- **Blast radius:** Only affects Edit tool output text; no behavior change for successful edits

## Test Plan

- [ ] Unit tests: trigger each error pattern, verify directive is appended
- [ ] Unit tests: successful edit output is unchanged
- [ ] Integration test: delegate session with intentionally stale edit, verify recovery

## Operational Impact

- **Metrics:** Count of edit error recoveries triggered per session
- **Logging:** Log at debug level when recovery directive is injected
- **Disk/CPU/Memory:** Negligible — string matching on tool output

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Edit Error Recovery | P0 | S | High — prevents most common delegate failure mode |

## Trade-offs

Alternative: block the retry entirely until a Read is observed. Rejected because it would require tracking tool call sequences and could break valid retry patterns where the agent mentally corrects its assumption. Appending a directive is less invasive and lets the model decide.

Inspiration: oh-my-openagent `src/hooks/edit-error-recovery/hook.ts`
