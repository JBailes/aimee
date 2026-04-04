# Proposal: Unstable Agent Babysitter

## Problem

Delegates can enter a "thinking loop" — producing extended thinking blocks but no tool calls or text output. They appear active but make no progress. This is different from simple idleness (proposal #8) or empty responses (proposal #22): the agent is consuming tokens and context window on thinking that never materializes into action. Without detection, these sessions burn through the entire context budget.

Evidence: oh-my-openagent implements an `unstable-agent-babysitter` hook (`src/hooks/unstable-agent-babysitter/`) that monitors background delegate sessions for instability signals. When it detects a delegate in a thinking loop (consecutive messages with thinking blocks but no tool calls or output), it sends a "nudge" prompt to get the delegate back on track, or escalates to the orchestrator.

## Goals

- Detect delegates in thinking loops (thinking without acting)
- Detect delegates producing consecutive empty or near-empty responses
- Nudge stuck delegates with a corrective prompt
- Escalate to orchestrator if nudge doesn't resolve the issue
- Configurable detection thresholds and cooldown

## Approach

Monitor delegate session message patterns. Track consecutive messages that have thinking content but no tool calls and no substantive text output. When the count exceeds a threshold, inject a nudge via the session API. If the delegate remains stuck after the nudge, abort and escalate.

### Detection signals

| Signal | Threshold | Action |
|--------|-----------|--------|
| Consecutive thinking-only messages | 3 | Inject nudge prompt |
| Nudge didn't help (still stuck) | 2 more | Abort and escalate |
| Consecutive empty responses | 2 | Inject nudge prompt |
| Total tool calls without progress | configurable | Inject nudge prompt |

### Nudge prompt

```
[STUCK DETECTION — TAKE ACTION NOW]
You appear to be thinking without acting. Stop deliberating and:
1. Make a tool call (Read, Edit, Bash, etc.)
2. If you're blocked, explain what's blocking you
3. If the task is done, report your results
Do NOT continue thinking without producing output.
```

### Changes

| File | Change |
|------|--------|
| `src/agent_eval.c` | Track message patterns per delegate; detect thinking loops |
| `src/agent_coord.c` | Inject nudge prompt or abort on detection |
| `src/headers/agent.h` | Add babysitter config and state structs |

## Acceptance Criteria

- [ ] 3 consecutive thinking-only messages triggers a nudge
- [ ] Nudge is injected as a user message in the delegate session
- [ ] Continued stuckness after nudge triggers abort + escalation
- [ ] Normal delegates (thinking then acting) are not affected
- [ ] Cooldown prevents multiple nudges in quick succession (5 min default)
- [ ] Detection thresholds are configurable

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2–3 days)
- **Dependencies:** Ability to inject messages into delegate sessions

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; active for delegate sessions
- **Rollback:** Remove monitoring; stuck delegates run until context exhaustion as before
- **Blast radius:** Only affects delegate sessions detected as stuck

## Test Plan

- [ ] Unit test: 3 thinking-only messages triggers nudge
- [ ] Unit test: 2 thinking-only messages does not trigger
- [ ] Unit test: thinking followed by tool call resets counter
- [ ] Unit test: post-nudge stuckness triggers abort
- [ ] Unit test: cooldown prevents rapid re-nudging
- [ ] Integration test: delegate with stuck-loop prompt, verify nudge and recovery

## Operational Impact

- **Metrics:** Nudge count, abort count, recovery rate after nudge
- **Logging:** Log nudge at warning, abort at error
- **Disk/CPU/Memory:** One counter per delegate session; negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Unstable Agent Babysitter | P2 | M | Medium — prevents stuck delegates from wasting context |

## Trade-offs

Alternative: hard timeout on delegate sessions instead of pattern detection. Simpler but less targeted — a delegate legitimately thinking about a complex problem would be killed. Pattern detection distinguishes between productive thinking (followed by action) and stuck thinking (repeated with no output).

Inspiration: oh-my-openagent `src/hooks/unstable-agent-babysitter/`
