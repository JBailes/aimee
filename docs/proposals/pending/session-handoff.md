# Proposal: Session Handoff / Context Export

## Problem

When a session approaches context limits, there's no clean way to continue work in a fresh session. The user must manually summarize what happened, what's done, and what remains. This is error-prone — important decisions, constraints, and in-progress state get lost in the handoff. Long-running tasks that span multiple sessions lose continuity.

Evidence: oh-my-openagent implements a `/handoff` command (`src/features/builtin-commands/templates/handoff.ts`) that creates a structured context summary: user requests verbatim, work completed, tasks remaining, decisions made, files modified, patterns established, and explicit constraints. This document can be pasted into a new session for seamless continuation.

## Goals

- Export a session's state as a structured handoff document
- Include: original task, work completed, remaining tasks, key decisions, modified files, constraints
- Handoff document is self-contained — new session can resume with no prior context
- Works for both orchestrator and delegate sessions

## Approach

Add an `aimee handoff` CLI command that gathers session state from multiple sources and produces a structured markdown document.

### Data sources

1. **Session task list**: current tasks and their status
2. **Git state**: `git diff --stat`, `git status`, recent commits
3. **Working memory**: active plan, key findings, decisions
4. **Agent state**: active delegates, their status

### Handoff document structure

```markdown
# Session Handoff

## Original Request
[Verbatim user request]

## Work Completed
- [File]: [Change description]
- ...

## Remaining Tasks
- [ ] [Task 1]
- [ ] [Task 2]

## Key Decisions
- [Decision 1]: [Rationale]

## Modified Files
[git diff --stat output]

## Constraints and Rules
- [Constraint from user or project config]

## How to Continue
Paste this document into a new session. The next step is: [specific next action]
```

### Changes

| File | Change |
|------|--------|
| `src/cmd_work.c` | Add `aimee handoff` subcommand |
| `src/working_memory.c` | Expose session state for handoff export |
| `src/tasks.c` | Expose task list serialization for handoff |

## Acceptance Criteria

- [ ] `aimee handoff` produces a valid markdown document
- [ ] Document includes all 7 sections (request, work, remaining, decisions, files, constraints, continuation)
- [ ] Git state is captured accurately
- [ ] Task list status is preserved
- [ ] Document is <4K tokens (fits in a system prompt)

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (2–3 days)
- **Dependencies:** Working memory and task system must be functional

## Rollout and Rollback

- **Rollout:** New CLI command; no changes to existing behavior
- **Rollback:** Remove command; no impact on existing functionality
- **Blast radius:** Additive — new command only

## Test Plan

- [ ] Unit test: handoff document includes all sections
- [ ] Unit test: git state is captured correctly
- [ ] Unit test: empty session produces minimal valid document
- [ ] Unit test: document fits within 4K token budget
- [ ] Integration test: complete a task, run handoff, verify completeness

## Operational Impact

- **Metrics:** Handoff frequency per user
- **Logging:** Log handoff generation at info level
- **Disk/CPU/Memory:** One-time document generation; negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Session Handoff | P2 | M | Medium — enables multi-session continuity |

## Trade-offs

Alternative: automatic handoff when context reaches 90%. Risky — may trigger at an inconvenient moment. Manual invocation is safer and gives the user control.

Alternative: persist full session transcripts for replay. Too expensive and noisy. A structured summary is more useful than a raw transcript.

Inspiration: oh-my-openagent `src/features/builtin-commands/templates/handoff.ts`
