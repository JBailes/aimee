# Proposal: Parallel Execution Waves for Plans

## Problem

Plan execution is currently sequential — tasks are dispatched one at a time. Many tasks within a plan are independent and could run in parallel (e.g., implementing tests for different modules, updating documentation across multiple files). Sequential execution wastes time and underutilizes available compute.

Evidence: oh-my-openagent's plan template (`src/agents/prometheus/plan-template.ts`) explicitly groups tasks into parallel "waves." Independent tasks within a wave run concurrently; the next wave starts only when the current wave completes. They target 5-8 tasks per wave and flag under-splitting (<3 per wave).

## Goals

- Group independent tasks into parallel execution waves
- Dispatch all tasks in a wave concurrently as separate delegates
- Gate on wave completion before starting the next wave
- Automatically detect task dependencies to determine wave boundaries

## Approach

Extend plan generation to produce a dependency graph. Topologically sort tasks and group independent ones into waves. During execution, dispatch all tasks in the current wave as parallel delegates. When all delegates in the wave complete, advance to the next wave.

### Dependency detection

- Explicit: task B references output of task A → B depends on A
- File-based: tasks modifying the same file are serialized
- Implicit: tasks in different directories/modules are assumed independent

### Wave example

```
Wave 1 (parallel): [Add tests for auth, Add tests for api, Update README]
Wave 2 (parallel): [Integrate auth changes, Integrate api changes]  -- depends on Wave 1
Wave 3 (sequential): [Run full test suite]  -- depends on Wave 2
```

### Changes

| File | Change |
|------|--------|
| `src/agent_plan.c` | Generate dependency graph and wave assignments |
| `src/agent_coord.c` | Dispatch wave of delegates; gate on wave completion |
| `src/headers/agent.h` | Add wave and dependency graph structures |

## Acceptance Criteria

- [ ] Plans include wave assignments for all tasks
- [ ] Independent tasks within a wave are dispatched concurrently
- [ ] Next wave starts only after all tasks in current wave complete
- [ ] File-conflict detection prevents concurrent edits to the same file
- [ ] Single-task waves are allowed for tasks with many dependencies
- [ ] Wave execution respects compute concurrency limits

## Owner and Effort

- **Owner:** aimee
- **Effort:** L (3–5 days)
- **Dependencies:** Delegate concurrency must be supported by the server

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; opt-in via plan config (`parallel: true`)
- **Rollback:** Disable parallel flag; plans execute sequentially as before
- **Blast radius:** Affects plan execution orchestration; individual delegate behavior unchanged

## Test Plan

- [ ] Unit test: independent tasks are grouped in same wave
- [ ] Unit test: dependent tasks are in different waves
- [ ] Unit test: same-file tasks are serialized
- [ ] Unit test: wave completion gates next wave
- [ ] Integration test: 3-task parallel wave, verify concurrent dispatch

## Operational Impact

- **Metrics:** Tasks per wave, wave count per plan, wall-clock time vs sequential
- **Logging:** Log wave dispatch and completion at info level
- **Disk/CPU/Memory:** Proportional to delegate count; bounded by concurrency limit

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Parallel Execution Waves | P3 | L | High — multiplies throughput for large plans |

## Trade-offs

Alternative: let the orchestrator decide parallelism ad-hoc. Less predictable and harder to reason about. Explicit wave structure makes the execution plan visible and debuggable.

Alternative: fine-grained task-level parallelism (start each task as soon as its dependencies are met). More efficient but significantly more complex to implement and debug. Waves are a good 80/20 solution.

Inspiration: oh-my-openagent `src/agents/prometheus/plan-template.ts`
