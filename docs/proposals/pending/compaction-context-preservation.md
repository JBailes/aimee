# Proposal: Compaction Context Preservation

## Problem

When delegate sessions hit context limits and compaction/summarization occurs, the summary is unstructured and often loses critical information: the original task, which files are being edited, plan progress, delegation state, and explicit constraints. The delegate continues with degraded context, repeating work or making decisions that contradict earlier findings.

Evidence: oh-my-openagent injects a structured compaction prompt (`src/hooks/compaction-context-injector/compaction-context-prompt.ts`) that tells the summarizer exactly what to preserve: user requests verbatim, final goal, work completed, remaining tasks, active files, explicit constraints, agent verification state, and delegated session IDs.

## Goals

- Ensure compaction summaries retain all critical session state
- Structured preservation template covers: task, files, progress, constraints, delegation
- Delegate sessions resume seamlessly after compaction without re-discovering context
- Template is injected automatically, requiring no agent cooperation

## Approach

When a compaction event is detected for a delegate session, inject a structured preservation directive into the summarization prompt. The directive specifies exactly which categories of information must survive compaction.

### Preservation categories

1. **Original task**: The exact prompt that started this delegate session
2. **Work completed**: Files created/modified, features implemented, problems solved
3. **Remaining tasks**: What still needs to be done from the original task
4. **Active files**: Paths of files currently being edited or frequently referenced
5. **Key findings**: Important discoveries, decisions made, constraints identified
6. **Delegation state**: Any sub-delegations in flight, their status and session IDs

### Changes

| File | Change |
|------|--------|
| `src/agent_context.c` | Inject preservation template on compaction events |
| `src/headers/agent.h` | Add compaction preservation template constant |

## Acceptance Criteria

- [ ] Compaction events trigger preservation template injection
- [ ] Template covers all 6 preservation categories
- [ ] Post-compaction summary includes structured sections for each category
- [ ] Delegate resumes working on the correct task after compaction
- [ ] Template is <500 tokens to minimize overhead

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (1–2 days)
- **Dependencies:** Context compaction must be implemented

## Rollout and Rollback

- **Rollout:** Compile-time inclusion; active for all sessions with compaction
- **Rollback:** Remove template injection; compaction uses default unstructured summarization
- **Blast radius:** Affects compaction summary quality only

## Test Plan

- [ ] Unit test: compaction event triggers template injection
- [ ] Unit test: template contains all 6 categories
- [ ] Integration test: delegate session compacts, verify post-compaction summary has structured sections
- [ ] Integration test: delegate resumes correct task after compaction

## Operational Impact

- **Metrics:** Post-compaction task resumption success rate
- **Logging:** Log compaction events and template injection at info level
- **Disk/CPU/Memory:** ~500 tokens added to compaction prompt; negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Compaction Context Preservation | P2 | M | Medium — prevents context loss during long sessions |

## Trade-offs

Alternative: pin critical context so it never gets compacted. More reliable but reduces the effectiveness of compaction (less context freed). Structured summarization is a better balance.

Inspiration: oh-my-openagent `src/hooks/compaction-context-injector/compaction-context-prompt.ts`
