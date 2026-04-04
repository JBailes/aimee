# Proposal: Empty Delegate Response Detector

## Problem

Delegates sometimes return empty responses — the session crashed, timed out, hit an API error, or silently failed. The orchestrator receives an empty string as the delegate's result and has no signal that anything went wrong. It may proceed as if the task was completed (interpreting silence as success) or get confused by the lack of output.

Evidence: oh-my-openagent implements an `empty-task-response-detector` (`src/hooks/empty-task-response-detector.ts`) that intercepts empty Task tool results and replaces them with a clear warning explaining what happened.

## Goals

- Detect when a delegate returns empty or whitespace-only output
- Replace with a clear warning explaining the delegate likely crashed
- Orchestrator can then retry or investigate
- Zero false positives — only trigger on genuinely empty responses

## Approach

In the MCP tool layer, after a delegate task tool returns, check if the output is empty or whitespace-only. If so, replace it with a diagnostic message.

### Warning message

```
[DELEGATE RETURNED EMPTY RESPONSE]
The delegate session returned no output. This typically means:
- The delegate crashed or hit an API error
- The session timed out
- The delegate failed to produce any result

Action: Retry the delegation, or investigate the delegate session logs.
```

### Changes

| File | Change |
|------|--------|
| `src/agent_coord.c` | Check delegate response for empty/whitespace; inject warning |

## Acceptance Criteria

- [ ] Empty delegate response is replaced with diagnostic warning
- [ ] Whitespace-only response is also caught
- [ ] Non-empty responses pass through unchanged
- [ ] Warning clearly indicates the delegate failed
- [ ] Works for both sync and async delegate responses

## Owner and Effort

- **Owner:** aimee
- **Effort:** Trivial (< 2 hours)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; always active
- **Rollback:** Remove check; empty responses pass through as before
- **Blast radius:** Only affects empty delegate responses

## Test Plan

- [ ] Unit test: empty string triggers warning
- [ ] Unit test: whitespace-only string triggers warning
- [ ] Unit test: non-empty response passes through unchanged
- [ ] Unit test: warning message contains diagnostic guidance

## Operational Impact

- **Metrics:** Count of empty delegate responses per session
- **Logging:** Log at warning level when empty response detected
- **Disk/CPU/Memory:** Negligible — one string length check per delegate response

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Empty Delegate Response Detector | P0 | Trivial | High — prevents silent delegate failures |

## Trade-offs

No significant alternatives — this is a straightforward defensive check with no downsides.

Inspiration: oh-my-openagent `src/hooks/empty-task-response-detector.ts`
