# Proposal: Orchestrator Self-Discipline and Anti-Duplication Guardrails

## Problem

The pending set has three variants of the same orchestration-quality issue:

- the orchestrator edits implementation files directly
- the orchestrator keeps working locally instead of delegating
- the orchestrator duplicates research already assigned to delegates

These are all failures of orchestrator discipline. They should share one proposal and one enforcement layer.

Evidence: oh-my-openagent's Atlas-style reminders and anti-duplication prompt sections both solve the same waste pattern: expensive orchestrators should coordinate, not re-implement or re-research delegated work.

## Goals

- Detect when the orchestrator behaves like an implementer instead of a coordinator.
- Warn on direct edits to non-planning source files.
- Warn when too many direct tool calls happen without delegation.
- Warn when new searches overlap with active delegate work.
- Keep all checks advisory, not blocking.

## Approach

Add one orchestrator-discipline guardrail with three signals:

1. direct edits to implementation files
2. excessive direct tool use without any delegation
3. search/read operations that overlap with pending delegate topics

When any signal fires, append a reminder that the orchestrator should delegate or wait for in-flight delegate work instead of duplicating it.

### Allowed paths (no reminder)

- `.aimee/*`
- `docs/proposals/*`
- Config files (`.yaml`, `.json`, `.toml` in project root)
- `CLAUDE.md`, `AGENTS.md`

### Changes

| File | Change |
|------|--------|
| `src/guardrails.c` | Add orchestrator discipline checks for edits, duplicate searches, and non-delegating behavior |
| `src/mcp_tools.c` | Call edit/search discipline checks in orchestrator sessions |
| `src/agent_eval.c` | Track direct tool-call count and inject one-time delegation nudges |
| `src/agent_coord.c` | Record pending delegation topics so overlap can be detected |

## Acceptance Criteria

- [ ] Orchestrator editing `src/foo.c` gets a delegation reminder appended
- [ ] Orchestrator editing `.aimee/plan-state.json` does NOT get a reminder
- [ ] Orchestrator editing `docs/proposals/bar.md` does NOT get a reminder
- [ ] Repeated direct tool usage without delegation triggers a one-time nudge
- [ ] Searches that significantly overlap with pending delegate topics trigger a warning
- [ ] Reminder is advisory (appended to output), not blocking
- [ ] Only orchestrator sessions trigger this — delegate sessions are unaffected

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (< 1 day)
- **Dependencies:** Session role (orchestrator vs delegate) must be distinguishable

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; active for orchestrator sessions
- **Rollback:** Remove the check; orchestrator edits freely as before
- **Blast radius:** Only affects orchestrator tool output text

## Test Plan

- [ ] Unit test: source file edit triggers reminder
- [ ] Unit test: config file edit does not trigger
- [ ] Unit test: delegate session edit does not trigger
- [ ] Integration test: orchestrator attempts direct code edit, verify reminder

## Operational Impact

- **Metrics:** Count of orchestrator direct-edit reminders per session
- **Logging:** Log at info level when reminder injected
- **Disk/CPU/Memory:** Negligible — path matching on tool output

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Orchestrator discipline guardrails | P1 | M | Medium — saves expensive orchestrator tokens |

## Trade-offs

Alternative: block orchestrator edits or duplicate searches entirely. Too rigid — there are legitimate exceptions. Reminder-only preserves flexibility while still steering behavior.

Inspiration: oh-my-openagent `src/hooks/atlas/system-reminder-templates.ts` (DIRECT_WORK_REMINDER)
