# Proposal: Orchestrator Self-Discipline

## Problem

The orchestrator session sometimes edits code files directly instead of delegating to a sub-agent. This wastes expensive orchestrator tokens (running on a top-tier model) on implementation work that a cheaper delegate model could handle. It also bypasses the delegation pipeline's guardrails, verification, and concurrency management.

Evidence: oh-my-openagent's Atlas hook (`src/hooks/atlas/system-reminder-templates.ts`) detects when the orchestrator edits files outside of plan/config directories and injects a reminder: "You are an ORCHESTRATOR, not an IMPLEMENTER. Delegate implementation work to subagents."

## Goals

- Detect when the orchestrator session directly edits non-config source files
- Inject a reminder to delegate instead
- Allow edits to plan files, config files, and `.aimee/` directory
- Advisory only — don't block the edit

## Approach

In the MCP tool layer, after an Edit/Write call from an orchestrator session, check if the target file is outside the allowed set (plans, configs, `.aimee/`). If so, append a delegation reminder to the tool output.

### Allowed paths (no reminder)

- `.aimee/*`
- `docs/proposals/*`
- Config files (`.yaml`, `.json`, `.toml` in project root)
- `CLAUDE.md`, `AGENTS.md`

### Changes

| File | Change |
|------|--------|
| `src/guardrails.c` | Add `guardrails_check_orchestrator_edit()` for non-config file edits |
| `src/mcp_tools.c` | Call guardrail check after Edit/Write in orchestrator sessions |

## Acceptance Criteria

- [ ] Orchestrator editing `src/foo.c` gets a delegation reminder appended
- [ ] Orchestrator editing `.aimee/plan-state.json` does NOT get a reminder
- [ ] Orchestrator editing `docs/proposals/bar.md` does NOT get a reminder
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
| Orchestrator Self-Discipline | P1 | S | Medium — saves expensive orchestrator tokens |

## Trade-offs

Alternative: block orchestrator edits entirely. Too rigid — there are legitimate cases (small fixes during plan writing). Reminder-only preserves flexibility.

Inspiration: oh-my-openagent `src/hooks/atlas/system-reminder-templates.ts` (DIRECT_WORK_REMINDER)
