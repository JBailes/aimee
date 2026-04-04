# Proposal: Session Compaction for Long-Running Agent Conversations

## Problem

Compaction-related work is currently spread across several narrow proposals:

- core session compaction
- context-pressure awareness
- structured preservation of critical state
- todo preservation across compaction
- tool-pair repair after compaction

Those are one lifecycle. If aimee compacts long-running sessions, it also needs to know when to compact, what must survive, how task state survives, and how message history remains valid afterward.

Long-running delegates and resumed sessions will otherwise lose task continuity, repeat work, or fail API validation after old tool state is summarized away.

## Goals

- Compact long sessions before they exceed practical model limits.
- Preserve critical state: task, remaining work, key files, findings, constraints, and todos.
- Surface context pressure before emergency failure.
- Keep message history valid after compaction, including tool-call pairing.
- Make resume and delegate continuation seamless.

## Approach

Implement one compaction pipeline with four parts:

1. token estimation and threshold checks
2. structured summary generation
3. state preservation and restoration
4. post-compaction history repair/validation

### Thresholds

- warn around 70% context usage
- checkpoint and prepare compaction around 80%
- compact before request construction would exceed configured limits

### Preserved State

- original task and user intent
- completed and remaining work
- active files and key findings
- explicit constraints and plan status
- task/todo list with statuses
- pending or recently completed tool activity needed for API correctness

### Changes

| File | Change |
|------|--------|
| `src/session_compact.c` | Token estimation, threshold checks, summary generation, iterative merge |
| `src/headers/session_compact.h` | Public compaction API |
| `src/agent.c` | Compact before oversized LLM calls and on resume |
| `src/agent_eval.c` | Context-pressure directives and threshold bookkeeping |
| `src/agent_context.c` | Inject preservation template and restore preserved state |
| `src/tasks.c` | Snapshot and restore todo/task state across compaction |
| `src/agent_context.c` | Validate/fill orphaned tool pairs after compaction |

## Acceptance Criteria

- [ ] Sessions compact based on configurable model-aware thresholds.
- [ ] Compacted summaries preserve task, progress, files, constraints, and key findings.
- [ ] Todo/task state is identical before and after compaction.
- [ ] Delegates receive context-pressure warnings before hard failure thresholds.
- [ ] Orphaned tool calls created by compaction are repaired before the next API request.
- [ ] Session resume loads compacted state safely when full history is too large.

## Owner and Effort

- **Owner:** delegate (code)
- **Effort:** L
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Start with transparent compaction for delegates and resume paths. Keep thresholds configurable and conservative.
- **Rollback:** Raise compaction thresholds or disable the compaction trigger path.
- **Blast radius:** Poor summaries or broken repair logic could degrade long-running sessions, so the state-preservation and repair steps are not optional extras.

## Test Plan

- [ ] Unit tests for token estimation, threshold detection, summary generation, iterative merge, and task snapshot/restore.
- [ ] Unit tests for orphaned tool-call repair after compaction.
- [ ] Integration tests for long delegate sessions and resumed oversized sessions.
- [ ] Manual verification of compaction notices in CLI and webchat.

## Operational Impact

- **Metrics:** `session_compactions_total`, `session_estimated_tokens`, `compaction_threshold_crossings_total`, `tool_pair_repairs_total`
- **Logging:** INFO for compaction events, DEBUG for state preservation and repair details
- **Alerts:** None
- **Disk/CPU/Memory:** Reduced context size at modest compaction overhead

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Core compaction library | P1 | M | High |
| Context-pressure awareness | P1 | S | High |
| State/todo preservation | P1 | S | High |
| Tool-pair repair after compaction | P1 | S | High |
| Resume integration | P2 | S | High |

## Trade-offs

- **Why merge the compaction proposals?** Partial compaction without preservation and repair is not shippable.
- **Why keep tool-pair repair here instead of standalone?** The failure only matters because compaction changes message history.
- **Why use structured preservation?** A generic summary is too lossy for multi-step coding work.
