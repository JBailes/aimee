# Proposal: Context Compaction Awareness

## Problem

Delegate sessions running long tasks approach their context window limit without any awareness. When compaction occurs, important context (plan details, discovered facts, intermediate results) can be lost. The delegate continues working with degraded context, producing lower-quality results or repeating work it already did.

Evidence: oh-my-openagent implements two related hooks: a context window monitor (`src/hooks/context-window-monitor.ts`) that injects a reminder at 70% usage ("you have context remaining — don't rush"), and a preemptive compaction hook (`src/hooks/preemptive-compaction.ts`) that triggers summarization at 78% to preserve critical context before the system forces compaction.

## Goals

- Track token usage per delegate session
- Warn delegates at 70% context usage: "context getting full, prioritize remaining work"
- At 80%: proactively checkpoint critical state (plan progress, key findings)
- Prevent delegates from rushing or skipping work due to perceived context pressure

## Approach

After each tool call response, extract token usage from the API response metadata. Track cumulative usage per session. At threshold crossings, inject system directives.

### Thresholds

| Usage | Action |
|-------|--------|
| 70% | Inject "context available — don't rush" reminder |
| 80% | Inject "checkpoint your progress now" directive |
| 90% | Inject "wrap up current task, report findings" directive |

### Changes

| File | Change |
|------|--------|
| `src/agent_context.c` | Track token usage from API response headers/metadata |
| `src/agent_eval.c` | Check thresholds after each turn; inject directives |
| `src/headers/agent.h` | Add token tracking fields to session state |

## Acceptance Criteria

- [ ] Token usage is tracked per session from API response metadata
- [ ] 70% threshold triggers a "don't rush" reminder (once per session)
- [ ] 80% threshold triggers a checkpoint directive
- [ ] Directives are injected as system messages, not user messages
- [ ] Thresholds are configurable

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2–3 days)
- **Dependencies:** API response must include token usage metadata

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; active for delegate sessions
- **Rollback:** Remove tracking; delegates run without context awareness as before
- **Blast radius:** Only affects delegate session system messages

## Test Plan

- [ ] Unit test: token tracking accumulates correctly
- [ ] Unit test: 70% threshold triggers reminder exactly once
- [ ] Unit test: 80% threshold triggers checkpoint directive
- [ ] Unit test: below-threshold sessions get no directives
- [ ] Integration test: delegate with large task, verify directives appear at correct points

## Operational Impact

- **Metrics:** Token usage per delegate session, threshold trigger counts
- **Logging:** Log threshold crossings at info level
- **Disk/CPU/Memory:** One counter per session; negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Context Compaction Awareness | P2 | M | Medium — improves long-running delegate quality |

## Trade-offs

Alternative: proactively compact at 78% like OmO does. Requires control over the compaction/summarization process, which Aimee may not have depending on the LLM provider. Directive injection is provider-agnostic.

Inspiration: oh-my-openagent `src/hooks/context-window-monitor.ts`, `src/hooks/preemptive-compaction.ts`
