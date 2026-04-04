# Proposal: Delegate Liveness Monitoring and Circuit Breakers

## Problem

Several pending proposals cover the same broad delegate-failure surface:

- delegates return empty output
- delegates think without acting
- delegates go idle with unfinished work
- delegates loop on identical tool calls

These are all liveness and stuck-agent problems. They should be one proposal with one monitoring model rather than four unrelated hooks.

## Goals

- Detect delegates that are stuck, idle, looping, or silently failed.
- Nudge delegates when recovery is likely.
- Escalate or abort when recovery fails.
- Surface failures clearly to the orchestrator instead of letting them look like success.

## Approach

Build one delegate liveness monitor with four signal types:

1. empty or whitespace-only final responses
2. thinking-only or near-empty message loops
3. idle time with incomplete work
4. repeated identical tool-call loops

### Responses

- replace empty final output with an explicit failure diagnostic
- inject a corrective prompt when a delegate appears stuck but recoverable
- escalate to orchestrator or abort after repeated failed recovery

### Detection Examples

- 3 consecutive thinking-only messages → nudge
- 30s idle with incomplete tasks → reminder
- 3 identical tool calls in a row → loop warning
- 5 identical tool calls in a row → abort
- empty final response → convert to failure diagnostic immediately

### Changes

| File | Change |
|------|--------|
| `src/agent_eval.c` | Track delegate liveness signals and thresholds |
| `src/agent_coord.c` | Inject nudges, replace empty responses, escalate/abort |
| `src/agent_tools.c` | Track identical tool-call signatures |
| `src/agent_jobs.c` | Idle timers for delegates with incomplete work |
| `src/tasks.c` | Incomplete-task checks for liveness decisions |

## Acceptance Criteria

- [ ] Empty delegate responses are replaced with explicit failure diagnostics.
- [ ] Thinking-only or near-empty loops trigger nudges before hard abort.
- [ ] Idle delegates with unfinished work are reminded or escalated.
- [ ] Repeated identical tool calls trigger a circuit breaker.
- [ ] Healthy delegates that think briefly and then act are unaffected.
- [ ] Thresholds and cooldowns are configurable.

## Owner and Effort

- **Owner:** aimee
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Start with empty-response detection and loop circuit breakers, then add more subjective stuckness heuristics.
- **Rollback:** Disable the liveness monitor and fall back to passive delegate behavior.
- **Blast radius:** Delegate sessions only.

## Test Plan

- [ ] Unit tests: empty response detection
- [ ] Unit tests: thinking-loop and idle-time thresholds
- [ ] Unit tests: identical-call circuit breaker behavior
- [ ] Integration tests: delegate gets nudged, recovers, or is escalated appropriately

## Operational Impact

- **Metrics:** `delegate_empty_responses`, `delegate_nudges_total`, `delegate_stall_escalations`, `delegate_circuit_breakers`
- **Logging:** WARN on nudges and empty responses, ERROR on aborts/escalations
- **Alerts:** None
- **Disk/CPU/Memory:** Small per-delegate counters/timers

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Empty-response detection | P1 | S | High |
| Loop circuit breaker | P1 | S | High |
| Idle/stuck delegate recovery | P1 | M | High |

## Trade-offs

- **Why merge these proposals?** They all describe different symptoms of the same problem: delegates that stop making useful progress.
- **Why nudge before aborting?** Many stalls are recoverable with one explicit correction.
- **Why keep thresholds configurable?** Different models and roles have different normal pacing.
