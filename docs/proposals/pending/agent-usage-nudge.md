# Proposal: Agent Usage Nudge

## Problem

The orchestrator sometimes does all the work itself — reading files, running commands, making edits — without delegating to sub-agents. This wastes expensive orchestrator tokens on work that cheaper delegates could handle. The orchestrator may not realize it has delegation capabilities, or it may fall into a pattern of "just doing it myself" for convenience.

Evidence: oh-my-openagent implements an `agent-usage-reminder` hook (`src/hooks/agent-usage-reminder/`) that tracks whether the orchestrator has used any subagents during a session. After multiple direct tool calls without delegation, it injects a reminder to consider delegating. The hook only applies to orchestrator agents, not subagents.

## Goals

- Detect when the orchestrator has made many tool calls without delegating
- Inject a nudge to consider delegation after a configurable threshold (default: 10 tool calls)
- Only nudge once per session to avoid annoyance
- Only applies to orchestrator sessions, not delegates

## Approach

Track tool call count per orchestrator session. When the count exceeds the threshold without any delegation calls, inject a one-time reminder.

### Nudge message

```
[DELEGATION REMINDER]
You've made N direct tool calls without delegating. Consider whether any of this
work could be handled by a delegate on a cheaper model. Delegation candidates:
- File reading and exploration
- Code review and analysis
- Implementation of well-defined changes
- Testing and verification
```

### Changes

| File | Change |
|------|--------|
| `src/agent_eval.c` | Track tool calls per session; inject nudge at threshold |
| `src/agent_coord.c` | Reset counter when delegation occurs |

## Acceptance Criteria

- [ ] 10+ tool calls without delegation triggers a nudge
- [ ] Nudge appears exactly once per session
- [ ] Delegation resets the counter (no nudge after delegation)
- [ ] Delegate sessions never receive the nudge
- [ ] Threshold is configurable

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (< 1 day)
- **Dependencies:** Session role tracking (orchestrator vs delegate)

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; active for orchestrator sessions
- **Rollback:** Remove tracking; orchestrator operates without nudges as before
- **Blast radius:** One additional system message per orchestrator session, at most

## Test Plan

- [ ] Unit test: 10 tool calls triggers nudge
- [ ] Unit test: 9 tool calls does not trigger
- [ ] Unit test: delegation at call 5 resets counter
- [ ] Unit test: nudge only fires once per session
- [ ] Unit test: delegate session does not receive nudge

## Operational Impact

- **Metrics:** Nudge trigger count, delegation rate before/after nudge
- **Logging:** Log nudge at info level
- **Disk/CPU/Memory:** One counter per session; negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Agent Usage Nudge | P1 | S | Medium — encourages efficient model usage |

## Trade-offs

Alternative: automatically delegate after the threshold. Too aggressive — the orchestrator may have good reasons for direct work (quick fixes, planning). Nudging preserves autonomy while raising awareness.

Inspiration: oh-my-openagent `src/hooks/agent-usage-reminder/`
