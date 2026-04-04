# Proposal: Todo Continuation Enforcer

## Problem

Delegate agents sometimes stop working before completing their assigned tasks. They produce a partial result, emit a final text response, and go idle — without marking remaining tasks as done or explicitly reporting failure. The orchestrator has no signal that the delegate gave up, and the incomplete work may not be discovered until later review.

Evidence: oh-my-openagent implements a todo continuation enforcer (`src/hooks/todo-continuation-enforcer/`) that monitors delegate sessions. If a delegate has incomplete todos and goes idle (no tool calls for N seconds), the system injects a "you still have pending work" reminder or escalates to the orchestrator.

## Goals

- Detect when a delegate goes idle with incomplete tasks
- Nudge the delegate to continue or explicitly report why it stopped
- Escalate to the orchestrator if the delegate remains idle after nudging
- Avoid false positives during legitimate thinking/planning pauses

## Approach

Track task completion state per delegate session. After each tool call completes, start a countdown timer. If no new tool call arrives within the timeout and there are incomplete tasks, inject a continuation reminder into the session. If the delegate still doesn't act after a second timeout, mark the delegation as "stalled" and notify the orchestrator.

### Changes

| File | Change |
|------|--------|
| `src/agent_jobs.c` | Add idle detection timer per delegate session |
| `src/tasks.c` | Add `tasks_has_incomplete()` check |
| `src/agent_eval.c` | Inject continuation reminder on idle timeout |

## Acceptance Criteria

- [ ] Delegate idle for >30s with incomplete tasks triggers a reminder
- [ ] Delegate idle for >60s after reminder triggers stall escalation
- [ ] Completed task lists do not trigger (delegate finished normally)
- [ ] Timer resets on each tool call
- [ ] Timeout values are configurable

## Owner and Effort

- **Owner:** aimee
- **Effort:** S–M (1–2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; timeout values in project config
- **Rollback:** Remove timers; delegates idle without intervention as before
- **Blast radius:** Only affects delegate sessions; orchestrator sessions unaffected

## Test Plan

- [ ] Unit test: idle with incomplete tasks triggers reminder
- [ ] Unit test: idle with all tasks complete does not trigger
- [ ] Unit test: tool call resets the timer
- [ ] Unit test: second timeout triggers stall escalation
- [ ] Integration test: delegate with partial completion, verify nudge and escalation

## Operational Impact

- **Metrics:** Count of nudges and stall escalations per delegate
- **Logging:** Log nudge at info, stall at warning
- **Disk/CPU/Memory:** One timer per active delegate session

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Todo Continuation Enforcer | P1 | S–M | Medium — prevents silent delegate abandonment |

## Trade-offs

Alternative: require delegates to explicitly signal completion (e.g., a `done` tool call). Cleaner contract but requires model compliance, which is unreliable. Timer-based detection works regardless of model behavior.

Inspiration: oh-my-openagent `src/hooks/todo-continuation-enforcer/`
