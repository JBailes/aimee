# Proposal: L3 Structured Failure Episodes

## Problem

L3 is defined as the historical/episodic tier with persistent storage and decay, but it is minimally used. The only L3 content comes from decisions (`kind='decision'`), and there is no structured recording of failure episodes. When a delegation fails, the error message is captured in `agent_log` but never synthesized into a reusable L3 episode that could prevent the same failure from recurring. The system learns *that* something failed (via `memory_demote_from_failures`) but not *why* or *how to avoid it*.

Evidence: `memory_promote_delegation_patterns()` synthesizes success patterns into L2 facts but failure patterns only get a one-line warning. The `decision_log` tracks outcomes but failed decisions don't feed back into episodic memory. L3 memories are only queried in the Constraints section of context assembly (`memory_context.c:226`).

## Goals

- Failed delegations produce structured L3 episodes with cause, context, and avoidance guidance.
- L3 episodes are surfaced when the agent is about to attempt something similar to a past failure.
- L3 utilization rises from near-zero to meaningful (measurable via `aimee memory stats`).

## Approach

### 1. Structured Failure Episode Format

When a delegation fails (agent_log.success=0) and the role+agent combo has accumulated 2+ failures in 14 days, synthesize an L3 episode:

```
Key: failure_episode_{role}_{agent}_{date}
Kind: episode
Content: "Delegation [{role}] via {agent} failed {N} times in 14d.
  Last error: {error}.
  Common context: {files/terms from recent windows}.
  Avoid: {extracted pattern from error}."
```

### 2. Episode Extraction in `memory_promote.c`

Add `memory_synthesize_failure_episodes()` called from `memory_run_maintenance()`. Logic:

```c
/* Query agent_log for role+agent combos with 2+ failures in 14 days */
SELECT role, agent_name, COUNT(*) as fails,
       GROUP_CONCAT(error, ' | ') as errors
FROM agent_log
WHERE success = 0
  AND created_at > datetime('now', '-14 days')
  AND error IS NOT NULL AND error != ''
GROUP BY role, agent_name
HAVING fails >= 2
```

For each combo, check if an L3 episode already exists for this period (avoid duplicates). If not, insert with confidence = 0.7.

### 3. Similarity Check Before Delegation

In `cmd_delegate.c` (or the delegation entry point), before dispatching, query L3 episodes matching the role:

```sql
SELECT content FROM memories
WHERE tier = 'L3' AND kind = 'episode'
  AND key LIKE 'failure_episode_' || ? || '_%'
  AND confidence > 0.3
ORDER BY created_at DESC LIMIT 3
```

Include matched episodes in the delegation context as warnings.

### Changes

| File | Change |
|------|--------|
| `src/memory_promote.c` | Add `memory_synthesize_failure_episodes()`, call from `memory_run_maintenance()` |
| `src/headers/aimee.h` | Add `FAILURE_EPISODE_WINDOW 14`, `FAILURE_EPISODE_MIN 2` constants |
| `src/cmd_delegate.c` | Query L3 failure episodes for the target role before dispatch |

## Acceptance Criteria

- [ ] Failed delegations with 2+ failures in 14 days produce an L3 episode memory
- [ ] `aimee memory list --tier L3` shows failure episodes after maintenance
- [ ] Delegation dispatch includes relevant L3 failure warnings in context
- [ ] Duplicate episodes for the same period are not created
- [ ] `aimee memory stats` shows non-zero L3 counts after failures occur

## Owner and Effort

- **Owner:** TBD
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** New logic in existing maintenance cycle. L3 episodes appear after next `memory_run_maintenance()` following failures.
- **Rollback:** Revert commit. Existing L3 episodes remain but no new ones are created. No schema changes required.
- **Blast radius:** Adds memories and delegation context. No changes to existing memory behavior.

## Test Plan

- [ ] Unit test: 2 failed delegations for same role+agent in 14 days produces an L3 episode
- [ ] Unit test: 1 failure does not produce an episode (below threshold)
- [ ] Unit test: duplicate episode not created on repeated maintenance cycles
- [ ] Integration test: delegation dispatch includes failure warning when matching L3 episode exists
- [ ] Manual: trigger failures, run maintenance, verify `aimee memory list --tier L3`

## Operational Impact

- **Metrics:** L3 episode count visible in `aimee memory stats`.
- **Logging:** Episode creation logged to stderr.
- **Alerts:** None.
- **Disk/CPU/Memory:** ~1 row per failure pattern per 14-day window. Negligible.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Failure episode synthesis | P2 | S | Makes L3 useful |
| Pre-delegation warning | P2 | S | Prevents repeat failures |

## Trade-offs

**Why 14 days and 2 failures?** A single failure could be transient (network, timeout). Two failures in a short window suggest a systematic issue. 14 days is short enough to be relevant, long enough to accumulate signal.

**Why not merge with anti-patterns?** Anti-patterns are prescriptive ("don't do X") and escalate to hard rules. Failure episodes are descriptive ("this happened, here's context") and decay naturally. They serve different purposes — anti-patterns block, episodes inform.
