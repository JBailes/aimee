# Proposal: Delegate Token Budget Limiting

## Problem

Delegate system prompts can become bloated when they include injected context: task descriptions, referenced file content, skill prompts, AGENTS.md content, and configuration. A bloated system prompt consumes a large fraction of the delegate's context window before it does any actual work, reducing the quality and length of its output.

Evidence: oh-my-openagent implements a `token-limiter` (`src/tools/delegate-task/token-limiter.ts`) that enforces a token budget for delegate system prompts. When the combined content exceeds the budget, it progressively truncates the least important segments (category prompts first, then skills, then agents context).

## Goals

- Enforce a maximum token budget for delegate system prompts (default: 20K tokens)
- Progressive truncation: cut least important content first
- Clear indicator when content is truncated
- Configurable budget per delegation role

## Approach

When building a delegate's system prompt, estimate the total token count. If it exceeds the budget, truncate segments in priority order:

1. First truncate: injected file content (least critical — delegate can Read them)
2. Then truncate: context/background information
3. Last truncate: core task description (most critical — never fully removed)

### Changes

| File | Change |
|------|--------|
| `src/agent_coord.c` | Add `limit_delegate_prompt()` before dispatching |
| `src/config.c` | Parse delegate token budget from project.yaml |

## Acceptance Criteria

- [ ] Delegate prompt >20K tokens is truncated to fit budget
- [ ] Task description is never truncated below 2K tokens
- [ ] Injected file content is truncated first
- [ ] Truncated sections have `[TRUNCATED]` indicator
- [ ] Budget is configurable per role

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (1–2 days)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; default budget 20K tokens
- **Rollback:** Remove budget enforcement; full prompts sent as before
- **Blast radius:** Affects delegate system prompt only; delegates with small prompts unaffected

## Test Plan

- [ ] Unit test: prompt under budget passes unchanged
- [ ] Unit test: prompt over budget is truncated
- [ ] Unit test: truncation priority order is correct
- [ ] Unit test: task description minimum (2K tokens) is preserved
- [ ] Integration test: delegate with large context injection, verify truncation

## Operational Impact

- **Metrics:** Prompt truncation events per delegation, bytes removed
- **Logging:** Log truncation at debug level with segment details
- **Disk/CPU/Memory:** Negligible — one pass over prompt string

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Delegate Token Budget | P3 | M | Medium — improves delegate output quality |

## Trade-offs

Alternative: limit the number of injected files/context items rather than total tokens. Simpler but less precise — a few large files might blow the budget while many small files would be under it.

Inspiration: oh-my-openagent `src/tools/delegate-task/token-limiter.ts`
