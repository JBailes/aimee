# Proposal: Delegation Error Recovery with Retry Guidance

## Problem

When the orchestrator dispatches a delegate with incorrect parameters (wrong role, missing fields, invalid prompt format), the error message is generic and unhelpful. The orchestrator often retries with the same broken parameters, wasting a round-trip. Common errors include: missing required fields, unknown delegation roles, malformed prompts, and mutually exclusive options.

Evidence: oh-my-openagent implements a `delegate-task-retry` hook (`src/hooks/delegate-task-retry/`) that pattern-matches known delegation errors and injects targeted fix guidance: "Error Type: missing_load_skills → Fix: Add load_skills=[] parameter."

## Goals

- Detect known delegation dispatch errors by pattern-matching the error output
- Inject specific, actionable fix guidance for each error type
- Orchestrator can self-correct on the next attempt without human intervention
- Cover at least the 5 most common delegation errors

## Approach

After a delegate dispatch tool returns an error, scan the output for known error patterns. For each match, append targeted guidance explaining what went wrong and how to fix it.

### Error patterns and guidance

| Error pattern | Guidance |
|---|---|
| Unknown role | "Use a valid role: code, review, deploy, validate, test, diagnose, execute, refactor" |
| Empty prompt | "Provide a non-empty prompt describing the task" |
| Prompt too short (<20 chars) | "Prompt is too brief — provide enough context for the delegate to work independently" |
| Conflicting options | "Options X and Y are mutually exclusive — use only one" |
| Missing required field | "Required field 'X' is missing — add it to the delegation call" |

### Changes

| File | Change |
|------|--------|
| `src/agent_coord.c` | Add post-dispatch error detection and guidance injection |
| `src/headers/agent.h` | Add delegation error pattern table |

## Acceptance Criteria

- [ ] At least 5 delegation error patterns are detected
- [ ] Each pattern has specific, actionable fix guidance
- [ ] Non-error delegation results are not affected
- [ ] Guidance is concise (<50 words per pattern)
- [ ] Works for all delegation roles

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (< 1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion, always active
- **Rollback:** Remove pattern matching; generic errors as before
- **Blast radius:** Only affects delegation error output text

## Test Plan

- [ ] Unit test: each error pattern triggers correct guidance
- [ ] Unit test: successful delegation is unchanged
- [ ] Unit test: unknown errors pass through without guidance
- [ ] Integration test: orchestrator dispatches with wrong role, verify guidance appears

## Operational Impact

- **Metrics:** Delegation error count by type
- **Logging:** Log error detection at debug level
- **Disk/CPU/Memory:** Negligible — string matching on error output

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Delegation Error Recovery | P1 | S | Medium — reduces delegation retry failures |

## Trade-offs

Alternative: validate parameters before dispatch and reject early. More robust but requires maintaining a validation schema that mirrors the server's acceptance logic, creating a duplication risk. Post-error guidance is simpler and self-updating (new errors get caught as they appear in output).

Inspiration: oh-my-openagent `src/hooks/delegate-task-retry/`
