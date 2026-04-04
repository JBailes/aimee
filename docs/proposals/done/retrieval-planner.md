# Proposal: Memory Retrieval Planner

## Problem

Memory retrieval is currently one-size-fits-all: every query runs the same FTS5 search + graph boost pipeline against the full memory corpus. The task-aware context assembly proposal (pending) improves *scoring* but not *strategy*. Different task intents need fundamentally different retrieval approaches:

- **Debugging** needs: failure episodes, procedures, recent changes, system topology
- **Planning** needs: decisions, constraints, policies, architecture facts
- **Code review** needs: conventions, preferences, past decisions in the same area
- **Deployment** needs: procedures, policies, environment facts, incident history

Without a planner, a "debug PostgreSQL auth" query retrieves the same memory pool as a "plan new API endpoint" query — they just get ranked differently. The retrieval planner decides *what to look for*, not just *how to rank what's found*.

Evidence: `memory_search()` takes a flat query string and returns results from all kinds and tiers. `memory_assemble_context()` fills fixed sections regardless of task type. There is no mechanism to say "for this task, I need procedures and episodes, not preferences."

## Goals

- A lightweight planner step decides which memory kinds, tiers, and retrieval strategies to use based on task intent.
- Retrieval budget is allocated to the most relevant memory classes for the task.
- The planner is rule-based and fast (<5ms), not model-based.
- Context assembly respects the planner's allocation.

## Approach

### 1. Task Intent Classification

Classify the task hint into one of a small set of intents using keyword matching:

```c
typedef enum {
    INTENT_DEBUG,    /* "fix", "bug", "error", "fail", "broken", "crash" */
    INTENT_PLAN,     /* "plan", "design", "architect", "propose", "new feature" */
    INTENT_REVIEW,   /* "review", "check", "audit", "convention", "style" */
    INTENT_DEPLOY,   /* "deploy", "release", "ship", "rollout", "migrate" */
    INTENT_GENERAL   /* fallback: no clear intent */
} task_intent_t;

task_intent_t classify_intent(const char *task_hint);
```

Classification is keyword-based with simple scoring: count matching keywords per intent, pick the highest. Ties go to `INTENT_GENERAL`.

### 2. Retrieval Plan

Each intent maps to a retrieval plan that specifies:

```c
typedef struct {
    task_intent_t intent;
    /* Budget allocation per kind (fractions summing to 1.0) */
    double kind_budget[8]; /* fact, preference, decision, episode, task, scratch, procedure, policy */
    /* Which tiers to search */
    int include_l0;        /* session scratch */
    int include_l3;        /* failure history */
    /* Recency bias (0.0 = no bias, 1.0 = strongly prefer recent) */
    double recency_weight;
    /* Minimum confidence threshold */
    double min_confidence;
} retrieval_plan_t;
```

Default plans:

| Intent | Facts | Prefs | Decisions | Episodes | Tasks | Scratch | Procedures | Policies | L0 | L3 | Recency |
|--------|-------|-------|-----------|----------|-------|---------|------------|----------|----|----|---------|
| Debug | 0.15 | 0.0 | 0.10 | 0.25 | 0.05 | 0.0 | 0.30 | 0.05 | No | Yes | 0.7 |
| Plan | 0.25 | 0.10 | 0.25 | 0.05 | 0.10 | 0.0 | 0.05 | 0.20 | No | No | 0.2 |
| Review | 0.15 | 0.25 | 0.20 | 0.10 | 0.0 | 0.0 | 0.10 | 0.20 | No | No | 0.3 |
| Deploy | 0.15 | 0.0 | 0.10 | 0.15 | 0.05 | 0.0 | 0.30 | 0.25 | No | Yes | 0.5 |
| General | 0.20 | 0.10 | 0.15 | 0.15 | 0.10 | 0.0 | 0.15 | 0.15 | No | No | 0.3 |

### 3. Budget-Aware Context Assembly

The retrieval plan feeds into context assembly. Instead of fixed section sizes:

```c
/* Current: fixed budgets */
#define SECTION_FACTS    2000
#define SECTION_TASKS    1500
#define SECTION_EPISODES 1200
#define SECTION_DECISIONS 800

/* Proposed: dynamic budgets from plan */
int budget_for_kind(retrieval_plan_t *plan, const char *kind, int total_budget) {
    int idx = kind_to_index(kind);
    return (int)(plan->kind_budget[idx] * total_budget);
}
```

This means a debug task allocates 30% of the 8000-char budget (2400 chars) to procedures and 25% (2000 chars) to episodes, while a planning task allocates 25% each to facts and decisions.

### 4. Recency-Weighted Scoring

The retrieval plan's `recency_weight` modifies the time decay formula:

```c
/* Current: fixed decay */
double decay = exp(-0.02 * days);

/* Proposed: intent-weighted decay */
double decay = exp(-0.02 * (1.0 + plan->recency_weight) * days);
```

Debug tasks (recency_weight=0.7) decay old memories faster, heavily preferring recent episodes. Planning tasks (recency_weight=0.2) are more even-handed.

### 5. Integration with Task-Aware Assembly

If the task-aware context assembly proposal is implemented, the retrieval planner sits upstream:

```
task_hint → classify_intent() → retrieval_plan_t → memory_assemble_context(db, task_hint, plan)
```

The planner decides *what to look for*; the task-aware scorer decides *which specific memories* within each budget.

### Changes

| File | Change |
|------|--------|
| `src/memory_context.c` | Accept `retrieval_plan_t`, use dynamic budgets, recency weighting |
| `src/headers/memory.h` | `task_intent_t`, `retrieval_plan_t`, `classify_intent()` |
| `src/headers/aimee.h` | Intent keyword lists (or move to config table) |
| `src/cmd_hooks.c` | Classify intent from task/prompt, pass plan to assembly |

## Acceptance Criteria

- [ ] `classify_intent("fix cert auth error")` returns `INTENT_DEBUG`
- [ ] `classify_intent("design new API endpoint")` returns `INTENT_PLAN`
- [ ] Debug intent allocates >50% of context budget to procedures + episodes
- [ ] Plan intent allocates >40% of context budget to facts + decisions + policies
- [ ] NULL task hint produces `INTENT_GENERAL` with balanced allocation
- [ ] Context output stays within `MAX_CONTEXT_TOTAL`
- [ ] Intent classification completes in <1ms
- [ ] Full pipeline (classify + assemble) completes in <50ms

## Owner and Effort

- **Owner:** TBD
- **Effort:** M
- **Dependencies:** Kind-specific lifecycle (for procedure/policy kinds). Complements task-aware context assembly but does not depend on it.

## Rollout and Rollback

- **Rollout:** Planner is inserted before context assembly. Default plans produce similar output to current fixed budgets for `INTENT_GENERAL`. No schema changes.
- **Rollback:** Revert commit. Assembly returns to fixed section budgets.
- **Blast radius:** Changes which memories appear in context based on task intent. The general intent preserves current-like behavior.

## Test Plan

- [ ] Unit test: each intent keyword set produces correct classification
- [ ] Unit test: mixed keywords (debug + plan) resolves by count
- [ ] Unit test: budget allocation sums to total budget for each plan
- [ ] Unit test: recency weighting changes time decay correctly
- [ ] Integration test: debug task context contains more episodes than planning task
- [ ] Integration test: deploy task context includes L3 failure warnings
- [ ] Manual: compare context output for same memory corpus across different intents

## Operational Impact

- **Metrics:** Intent classification distribution in `aimee memory stats`.
- **Logging:** Classified intent logged at info level per context assembly.
- **Alerts:** None.
- **Disk/CPU/Memory:** Keyword matching is O(n) on keyword lists (~50 total keywords). Negligible.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Intent classification | P1 | S | Enables everything else |
| Dynamic budget allocation | P1 | M | Core value |
| Recency weighting | P2 | S | Refinement |

## Trade-offs

**Why rule-based instead of model-based classification?** A model-based classifier would be more accurate but adds latency (external call) and a dependency. Keyword matching handles the common case — tasks almost always contain signal words ("fix", "plan", "deploy"). The 5-intent taxonomy is coarse enough that keyword overlap between intents is manageable. If misclassification becomes a problem, the keyword lists are editable without code changes.

**Why not let the agent specify intent directly?** Agents don't currently pass intent metadata. The planner infers intent from the task hint, which is already available. If a future protocol change adds explicit intent, the planner can accept it as an override.

**Why budget fractions instead of absolute sizes?** Fractions scale automatically if `MAX_CONTEXT_TOTAL` changes. They also make the trade-offs explicit: giving 30% to procedures means taking it from somewhere else.
