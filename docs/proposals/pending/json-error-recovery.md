# Proposal: JSON Parse Error Recovery

## Problem

LLM agents sometimes produce malformed JSON in tool call arguments: trailing commas, unescaped quotes, unclosed braces, or invalid escape sequences. When the tool layer fails to parse the JSON, the error message is typically a raw parse error ("unexpected token at position 42") which doesn't help the agent fix the problem. The agent often retries with the same broken JSON.

Evidence: oh-my-openagent implements a `json-error-recovery` hook (`src/hooks/json-error-recovery/hook.ts`) that detects JSON parse errors in tool outputs and injects a corrective reminder: "You sent invalid JSON arguments. STOP, look at the error, correct your syntax, and retry."

## Goals

- Detect JSON parse errors in tool call results
- Inject clear corrective guidance: what went wrong and how to fix it
- Exclude tools where JSON errors are expected in output (bash, grep, read)
- Prevent duplicate injection if the error already has a reminder

## Approach

After any tool call returns, check if the output contains JSON parse error patterns. If so (and the tool isn't in the exclude list), append a corrective reminder.

### Error patterns

- `json parse error`
- `failed to parse json`
- `invalid json`
- `malformed json`
- `unexpected end of json input`
- `unexpected token` (in JSON context)

### Excluded tools (JSON errors in their output are normal)

- bash, read, grep, glob, webfetch

### Changes

| File | Change |
|------|--------|
| `src/mcp_tools.c` | Add post-tool JSON error detection; append corrective reminder |
| `src/headers/mcp_tools.h` | JSON error pattern list and exclude list |

## Acceptance Criteria

- [ ] JSON parse errors in non-excluded tools trigger corrective reminder
- [ ] Excluded tools (bash, read, grep) do not trigger
- [ ] Reminder is injected at most once per tool call output
- [ ] Successful tool calls are not affected
- [ ] Reminder includes specific guidance: "check braces, quotes, trailing commas"

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (< 1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion, always active
- **Rollback:** Remove detection; raw parse errors as before
- **Blast radius:** Only affects tool output text on JSON errors

## Test Plan

- [ ] Unit test: JSON parse error triggers reminder
- [ ] Unit test: excluded tool does not trigger
- [ ] Unit test: non-JSON error does not trigger
- [ ] Unit test: duplicate injection is prevented
- [ ] Integration test: agent sends malformed JSON, verify corrective guidance

## Operational Impact

- **Metrics:** JSON error recovery count per session
- **Logging:** Log at debug level
- **Disk/CPU/Memory:** Negligible — regex matching on output strings

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| JSON Error Recovery | P1 | S | Medium — common failure mode for cheaper models |

## Trade-offs

Alternative: fix the JSON before passing to the tool (auto-repair). Risky — auto-repair may silently change the intended arguments. Better to let the agent fix it with guidance.

Inspiration: oh-my-openagent `src/hooks/json-error-recovery/hook.ts`
