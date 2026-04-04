# Proposal: Subagent Depth and Spawn Limits

## Problem

Without limits, delegates can spawn sub-delegates, which spawn sub-sub-delegates, creating unbounded delegation chains. Each level adds token cost and latency. A runaway delegation loop (A delegates to B, B delegates to C, C delegates back to A-like work) can exhaust the compute budget rapidly. Even without loops, deeply nested delegation makes debugging and progress tracking nearly impossible.

Evidence: oh-my-openagent enforces both depth limits (default: 3 levels) and spawn budgets (default: 50 total descendants per root session) in `src/features/background-agent/subagent-spawn-limits.ts`. It walks the session parent chain to determine depth and tracks total spawn count per root session.

## Goals

- Enforce a maximum delegation nesting depth (default: 3)
- Enforce a maximum total delegate count per root session (default: 50)
- Clear error message when limits are hit
- Configurable via project config

## Approach

Track delegation depth by maintaining a parent chain per session. When a delegate attempts to spawn a sub-delegate, check the current depth against the limit. Also maintain a per-root-session spawn counter. If either limit is exceeded, reject the delegation with a clear error.

### Changes

| File | Change |
|------|--------|
| `src/agent_coord.c` | Track delegation depth and total spawn count; enforce limits |
| `src/server_session.c` | Add `parent_session_id` and `root_session_id` to session state |
| `src/config.c` | Parse depth/spawn limits from project.yaml |

## Acceptance Criteria

- [ ] Delegation at depth 4 (with default limit 3) is rejected with clear error
- [ ] 51st delegate spawn (with default limit 50) is rejected with clear error
- [ ] Error message explains the limit and suggests alternatives
- [ ] Limits are configurable via `.aimee/project.yaml`
- [ ] Depth tracking correctly handles chains (A→B→C = depth 3)

## Owner and Effort

- **Owner:** aimee
- **Effort:** S–M (1–2 days)
- **Dependencies:** Session parent tracking

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; defaults active immediately
- **Rollback:** Remove limits; unlimited delegation depth/count as before
- **Blast radius:** Only affects delegation requests that exceed limits

## Test Plan

- [ ] Unit test: depth 1, 2, 3 allowed; depth 4 rejected
- [ ] Unit test: spawn count 49, 50 allowed; 51 rejected
- [ ] Unit test: different root sessions have independent spawn counts
- [ ] Unit test: custom limits from config are respected
- [ ] Integration test: delegate attempts to spawn sub-delegate beyond limit

## Operational Impact

- **Metrics:** Delegation rejections per session (depth vs spawn)
- **Logging:** Log rejection at warning level with depth/count details
- **Disk/CPU/Memory:** One counter per root session, one depth field per session

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Subagent Depth/Spawn Limits | P1 | S–M | High — prevents runaway delegation chains |

## Trade-offs

Alternative: soft limits (warn but allow). Insufficient — runaway delegation is expensive enough that a hard limit is warranted. The defaults (depth 3, count 50) are generous.

Inspiration: oh-my-openagent `src/features/background-agent/subagent-spawn-limits.ts`
