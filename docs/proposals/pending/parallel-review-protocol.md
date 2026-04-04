# Proposal: 5-Agent Parallel Review Protocol

## Problem

Proposal #21 (delegate-verification-protocol) has the orchestrator verify delegate work sequentially, covering diff review, automated checks, and scope validation. This is thorough but slow — the orchestrator does all verification itself on an expensive model. For large changes, comprehensive review covering goal verification, code quality, security, QA, and context mining is too much for a single pass.

Evidence: oh-my-openagent implements a `review-work` skill (`src/features/builtin-skills/skills/review-work.ts`) that launches 5 specialized review agents in parallel: (1) Goal Verifier, (2) QA Executor, (3) Code Reviewer, (4) Security Auditor, (5) Context Miner. All 5 must pass for the review to pass.

## Goals

- Launch parallel review delegates covering complementary concerns
- All review delegates must pass for the overall review to pass
- Review delegates run on cheaper models (they're read-only analysis)
- Total review time bounded by the slowest delegate, not sum of all
- Works as an `aimee review` command or automatic post-plan step

## Approach

Add an `aimee review` command that dispatches 5 parallel review delegates. Each delegate receives the git diff, changed file contents, and the original task description. They report pass/fail with specific findings.

### Review agents

| Agent | Focus | Key question |
|-------|-------|-------------|
| Goal Verifier | Requirements | Did we build exactly what was asked? |
| QA Executor | Functionality | Does it actually work? (runs tests, tries the code) |
| Code Reviewer | Quality | Is the code well-written? (patterns, readability, maintainability) |
| Security Auditor | Security | Any vulnerabilities? (injection, auth, data exposure) |
| Scope Checker | Scope | Did it touch things outside the task spec? |

### Changes

| File | Change |
|------|--------|
| `src/cmd_work.c` | Add `aimee review` command |
| `src/agent_coord.c` | Dispatch 5 parallel review delegates; aggregate results |
| `src/headers/agent.h` | Add review protocol structures |

## Acceptance Criteria

- [ ] `aimee review` dispatches 5 parallel review delegates
- [ ] Each delegate receives diff + file contents + task description
- [ ] All 5 must report "pass" for overall review to pass
- [ ] Any "fail" produces a summary of findings
- [ ] Total wall-clock time is bounded by slowest delegate
- [ ] Review delegates use read-only tools (no edits)

## Owner and Effort

- **Owner:** aimee
- **Effort:** L (3–5 days)
- **Dependencies:** Delegation system, parallel dispatch, git diff

## Rollout and Rollback

- **Rollout:** New CLI command; optional post-plan step in config
- **Rollback:** Remove command; manual review as before
- **Blast radius:** Additive — new command only

## Test Plan

- [ ] Unit test: 5 delegates are dispatched in parallel
- [ ] Unit test: all-pass produces overall pass
- [ ] Unit test: one fail produces overall fail with findings
- [ ] Unit test: review delegates receive correct inputs (diff, files, task)
- [ ] Integration test: implement a change, run review, verify results

## Operational Impact

- **Metrics:** Review pass/fail rate, findings per review agent, review time
- **Logging:** Log dispatch and results at info level
- **Disk/CPU/Memory:** 5 concurrent delegate sessions; bounded by concurrency limits

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Parallel Review Protocol | P3 | L | High — comprehensive automated review |

## Trade-offs

Alternative: single review delegate covering all concerns. Cheaper but less thorough — a single delegate tends to focus on one concern and miss others. Parallel specialized delegates catch complementary issues.

Alternative: sequential review phases. More thorough per-phase but much slower. Parallel execution is 5x faster at the cost of 5x compute (which is cheap on review-focused models).

Inspiration: oh-my-openagent `src/features/builtin-skills/skills/review-work.ts`
