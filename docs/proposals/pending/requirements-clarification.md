# Proposal: Structured Requirements Clarification

## Problem

When the primary agent receives a vague or ambiguous task, it either guesses at intent and builds the wrong thing, or asks an unstructured series of questions that may miss critical ambiguities. There is no systematic process to refine requirements before planning begins.

This leads to:
- Wasted implementation tokens when the agent builds something that doesn't match intent
- Incomplete plans that miss edge cases because scope was never clarified
- No record of *what was clarified and what was assumed* — making it hard to trace why a plan went wrong

oh-my-codex's `$deep-interview` skill addresses this with a structured Socratic interview: staged questioning (intent → feasibility → brownfield context), quantitative ambiguity scoring, and a readiness gate that blocks handoff to planning until clarity thresholds are met. The key insight is that clarification is a *measurable* process, not an open-ended conversation.

Evidence:
- Aimee's plan mode has no pre-planning clarification step
- The `aimee delegate` system can route `explain` and `reason` tasks but has no "interview the user" workflow
- Session context includes project memories but not "open questions" or "unresolved assumptions"
- No existing proposal covers requirements clarification

## Goals

- Ambiguous tasks go through a structured clarification phase before planning, reducing rework
- Clarification state is persisted (in the DB, not ephemeral) so it survives session boundaries
- An ambiguity score quantifies readiness — the agent and user can see how much is still unclear
- Clarification produces a spec artifact that feeds directly into plan creation
- The clarification flow works both interactively (user answers questions) and with delegate-assisted research (agent gathers context from codebase/docs)

## Approach

### 1. Clarification session model

Add a `clarifications` table to track clarification sessions:

```sql
CREATE TABLE IF NOT EXISTS clarifications (
    id INTEGER PRIMARY KEY,
    task TEXT NOT NULL,
    status TEXT DEFAULT 'active',  -- active, ready, abandoned
    ambiguity_score REAL DEFAULT 1.0,  -- 0.0 = fully clear, 1.0 = fully ambiguous
    threshold REAL DEFAULT 0.20,  -- readiness threshold
    rounds INTEGER DEFAULT 0,
    max_rounds INTEGER DEFAULT 12,
    spec TEXT,  -- crystallized spec (markdown), written when status='ready'
    dimensions TEXT DEFAULT '{}',  -- JSON: {intent, outcome, scope, constraints, success_criteria}
    transcript TEXT DEFAULT '[]',  -- JSON array of {round, question, answer, dimension, score_delta}
    created_at TEXT DEFAULT (datetime('now')),
    updated_at TEXT DEFAULT (datetime('now'))
);
```

### 2. Ambiguity scoring

Score is a weighted average across 5 dimensions, each scored 0.0–1.0:

| Dimension | Weight | What it measures |
|-----------|--------|-----------------|
| Intent | 0.30 | What the user actually wants to achieve |
| Outcome | 0.25 | What "done" looks like, measurable success criteria |
| Scope | 0.20 | What's in and out of scope, explicit non-goals |
| Constraints | 0.15 | Technical/resource/timeline constraints |
| Success criteria | 0.10 | How to verify the result is correct |

Scoring is delegated: after each user answer, a `reason` delegate scores the updated dimensions given the full transcript. This avoids baking scoring heuristics into C code.

### 3. Clarification loop

Implement `agent_clarify()` in a new `agent_clarify.c`:

```c
typedef struct {
    int id;
    char task[1024];
    double ambiguity_score;
    double threshold;
    int rounds;
    int max_rounds;
    double dimensions[5];  // intent, outcome, scope, constraints, success
    char status[16];
} clarification_t;

int agent_clarify_start(app_ctx_t *ctx, const char *task, double threshold);
int agent_clarify_round(app_ctx_t *ctx, int clar_id, const char *answer);
int agent_clarify_score(app_ctx_t *ctx, int clar_id);  // delegate scoring
int agent_clarify_crystallize(app_ctx_t *ctx, int clar_id);  // produce spec
```

Each round:
1. Identify the weakest dimension (highest ambiguity sub-score)
2. Generate one targeted question for that dimension (via `reason` delegate with the transcript as context)
3. Present the question to the user
4. Record the answer in the transcript
5. Re-score all dimensions via delegate
6. If `ambiguity_score < threshold` → ready for handoff
7. If `rounds >= max_rounds` → present current state, let user decide

### 4. Spec crystallization

When the clarification session reaches `ready`, crystallize a markdown spec:

```markdown
# Spec: <task summary>

## Intent
<what the user wants>

## Outcome
<definition of done>

## Scope
- In scope: ...
- Out of scope: ...

## Constraints
- ...

## Success Criteria
- [ ] ...

## Assumptions
- ...

## Source
Clarification session #N, N rounds, ambiguity score: 0.18
```

This spec can be:
- Stored as the `spec` column in the clarification row
- Injected into session context when creating a plan
- Fed directly to `agent_plan_create()` as the task description

### 5. CLI commands

```bash
aimee clarify "vague task description"           # start interactive clarification
aimee clarify "task" --quick                      # max 5 rounds, threshold 0.30
aimee clarify "task" --deep                       # max 20 rounds, threshold 0.15
aimee clarify status <id>                         # show current dimensions and score
aimee clarify spec <id>                           # output the crystallized spec
```

### 6. MCP tools

```json
{
  "name": "clarify_start",
  "description": "Start a structured clarification session for an ambiguous task",
  "parameters": {
    "task": "string",
    "threshold": "number (default 0.20)",
    "max_rounds": "number (default 12)"
  }
}
```

```json
{
  "name": "clarify_answer",
  "description": "Submit an answer to the current clarification question",
  "parameters": {
    "clarification_id": "integer",
    "answer": "string"
  }
}
```

### 7. Integration with plan mode

When `aimee plan` is active and the primary agent creates a plan, the PreToolUse hook can check whether the task has a completed clarification session. If not, and the task description is short/vague (heuristic: <50 words, no file paths or function names), suggest clarification before planning.

This is advisory, not blocking — the agent can proceed without clarification for concrete tasks.

### Changes

| File | Change |
|------|--------|
| `src/agent_clarify.c` | New: clarification loop, scoring delegation, spec crystallization |
| `src/headers/agent_clarify.h` | New: clarification types and function declarations |
| `src/cmd_core.c` | Add `clarify` subcommand routing |
| `src/mcp_tools.c` | Add `clarify_start` and `clarify_answer` MCP tools |
| `src/mcp_server.c` | Register new tools |
| `src/cmd_hooks.c` | Advisory check in PreToolUse: suggest clarification for vague plans |
| `src/db.c` | Add `clarifications` table migration |
| `src/tests/test_clarify.c` | Tests for scoring, round progression, crystallization |

## Acceptance Criteria

- [ ] `aimee clarify "task"` starts an interactive session and asks targeted questions
- [ ] Each round targets the weakest ambiguity dimension
- [ ] Ambiguity score decreases as answers are provided
- [ ] Session reaches `ready` when score drops below threshold
- [ ] `aimee clarify spec <id>` outputs a structured markdown spec
- [ ] Clarification state persists across sessions (DB-backed)
- [ ] `clarify_start` and `clarify_answer` MCP tools are callable
- [ ] Scoring uses delegate routing (cheapest `reason`-capable delegate)
- [ ] Fallback: if no delegate available, skip scoring and use round count as the only gate

## Owner and Effort

- **Owner:** aimee
- **Effort:** L (4-6 focused sessions)
- **Dependencies:** None, though composes well with consensus-planning proposal

## Rollout and Rollback

- **Rollout:** New table, new commands. No changes to existing behavior. Clarification is opt-in.
- **Rollback:** Revert commit. Drop `clarifications` table. No impact on existing data.
- **Blast radius:** None — entirely additive. Existing plan and delegate workflows are unchanged.

## Test Plan

- [ ] Unit tests: ambiguity score calculation from dimension sub-scores
- [ ] Unit tests: round progression — question targets weakest dimension
- [ ] Unit tests: crystallization produces valid markdown spec
- [ ] Unit tests: threshold gate — session transitions to `ready` at correct score
- [ ] Integration tests: end-to-end clarify → spec → plan create
- [ ] Failure injection: delegate unavailable during scoring — graceful fallback to round-count gate
- [ ] Manual verification: run `aimee clarify` on a vague task, observe questions improve specificity

## Operational Impact

- **Metrics:** `clarification_sessions_started`, `clarification_rounds_total`, `clarification_sessions_completed`, `clarification_avg_score_at_handoff`
- **Logging:** Round progress to stderr: `aimee: clarify #N round 3/12, score 0.45 (intent=0.2, outcome=0.7, scope=0.5, ...)`
- **Alerts:** None
- **Disk/CPU/Memory:** 1-2 delegate calls per round (question generation + scoring). Transcript stored as JSON, typically <10KB per session.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Clarification table + loop | P1 | M | High — core mechanism |
| Delegate-based scoring | P1 | S | High — makes scores meaningful |
| Spec crystallization | P1 | S | High — connects to plan creation |
| CLI commands | P2 | S | Medium — user interface |
| MCP tools | P2 | S | Medium — agent-callable |
| Plan mode advisory hook | P3 | S | Low — nice-to-have nudge |

## Trade-offs

**Why delegate scoring instead of heuristic scoring in C?**
Heuristic scoring (keyword matching, answer length) would be brittle and low-quality. A delegate with the full transcript can genuinely assess whether ambiguity has been resolved. The cost is 1 cheap delegate call per round — acceptable given the tokens saved by avoiding misaligned implementations.

**Why not use the existing memory system for clarification state?**
Clarification sessions have structured state (dimensions, scores, transcripts) that doesn't fit the key-value memory model. A dedicated table is cleaner and supports efficient queries like "find the clarification session for this task."

**Why advisory rather than mandatory before planning?**
Many tasks are concrete enough to plan directly ("fix the null pointer in memory.c line 42"). Mandatory clarification for those would be friction without value. The heuristic (short description, no concrete anchors) catches the cases that genuinely need it.

**Why not just ask the user to write a better prompt?**
The interview process *helps the user discover what they don't know they don't know*. Each question is targeted at a specific blind spot. This is more effective than "please be more specific."
