# Proposal: Delegate Verification Protocol

## Problem

Delegates frequently report "task complete" when the work is actually broken: stubs left behind, tests pass trivially, logic errors, scope creep, or silently added features nobody asked for. The orchestrator trusts the delegate's claim and moves on, leaving broken code in the tree. This is the single most common failure mode in multi-agent workflows.

Evidence: oh-my-openagent's `VERIFICATION_REMINDER` (`src/hooks/atlas/system-reminder-templates.ts`) explicitly states: "THE SUBAGENT JUST CLAIMED THIS TASK IS DONE. THEY ARE PROBABLY LYING." It mandates a 3-phase verification: (1) read every changed file before running anything, (2) run automated checks, (3) hands-on QA for user-facing changes.

## Goals

- After every delegate completion, inject a verification protocol into the orchestrator
- Verification covers: diff review, file reading, automated checks, QA
- Orchestrator cannot mark a plan task as done until verification passes
- Catch common delegate lies: stubs, placeholder TODOs, trivial tests, scope creep

## Approach

When a delegate reports completion, inject a structured verification prompt into the orchestrator's next turn. The prompt mandates specific steps before the orchestrator can proceed.

### Verification protocol

```
[DELEGATE COMPLETED — VERIFY BEFORE PROCEEDING]

Phase 1: READ THE CODE (before running anything)
1. git diff --stat to see which files changed
2. Read EVERY changed file — no skimming
3. For each file: Does it actually do what the task required?
4. Grep for TODO, FIXME, HACK, placeholder, stub in changed files

Phase 2: RUN AUTOMATED CHECKS
1. Run aimee git verify (build + tests + lint)
2. Check for new warnings or errors

Phase 3: CONFIRM SCOPE
1. Did the delegate touch files outside the task spec?
2. Did it add features not requested?
3. Does the change match the plan task description?

Do NOT mark this task complete until all phases pass.
```

### Changes

| File | Change |
|------|--------|
| `src/agent_coord.c` | Inject verification protocol after delegate completion |
| `src/headers/agent.h` | Add verification protocol template constant |

## Acceptance Criteria

- [ ] Every delegate completion triggers verification protocol injection
- [ ] Protocol includes all 3 phases (read, automate, scope)
- [ ] Orchestrator sees the protocol before its next action
- [ ] Protocol is concise (<200 words) to minimize context cost
- [ ] Works for both sync and async delegate completions

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (< 1 day)
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; active for all delegate completions
- **Rollback:** Remove injection; orchestrator trusts delegate claims as before
- **Blast radius:** Only affects orchestrator context after delegate completions

## Test Plan

- [ ] Unit test: delegate completion triggers protocol injection
- [ ] Unit test: protocol contains all 3 phases
- [ ] Unit test: non-delegate tool completions don't trigger
- [ ] Integration test: delegate completes with stubs, verify orchestrator catches them

## Operational Impact

- **Metrics:** Verification pass/fail rate, issues caught per delegate completion
- **Logging:** Log protocol injection at debug level
- **Disk/CPU/Memory:** ~200 tokens per delegate completion; negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Delegate Verification Protocol | P0 | S | High — catches the most common multi-agent failure mode |

## Trade-offs

Alternative: automatically run `aimee git verify` after every delegate completion. More mechanical but doesn't catch logic errors, scope creep, or stubs that pass compilation. Human-in-the-loop verification (via the orchestrator) catches a broader class of issues.

Alternative: require delegates to include a self-verification section. Unreliable — the same model that produced buggy code will claim its verification passed.

Inspiration: oh-my-openagent `src/hooks/atlas/system-reminder-templates.ts` (VERIFICATION_REMINDER)
