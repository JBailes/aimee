# Proposal: Planning Preparation and Review Pipeline

## Problem

The pending set currently treats planning as four separate concerns:

- clarify vague requirements
- classify task complexity to choose planning depth
- validate plan references mechanically
- run adversarial or multi-delegate plan review

These are all part of one planning-preparation pipeline. A strong plan should:

1. start from clarified requirements when needed
2. choose an appropriate planning depth
3. validate references and executability before execution
4. optionally pass review before implementation begins

Keeping these as separate proposals duplicates state and leaves later pipeline proposals redefining the same gates.

## Goals

- Ambiguous tasks can be clarified before planning.
- Trivial tasks can skip heavyweight planning.
- Generated plans can be mechanically validated before execution.
- Plans can be reviewed and contested before implementation.
- The pipeline produces one reviewable spec/plan artifact for later execution stages.

## Approach

Implement one planning-preparation pipeline with four stages:

1. request classification
2. clarification when needed
3. plan generation plus mechanical validation
4. optional plan review and revision loop

### Request Classification

Before invoking heavy planning, classify the task:

- `trivial`: execute directly or with minimal planning
- `simple`: lightweight plan
- `complex`: full planning and review pipeline

Classification should be heuristic-first and fast, with override flags such as `--plan-depth`.

### Clarification

For vague tasks, run a structured clarification flow with:

- persisted clarification state
- ambiguity scoring
- targeted questions against the weakest dimension
- spec crystallization when clarity is good enough

### Plan Validation

After a plan is generated, run a mechanical validation pass:

- referenced files exist
- referenced symbols exist
- tasks have enough starting context
- contradictions or obviously conflicting instructions are flagged

Mostly-valid plans should pass with warnings; clearly broken plans should be sent back for revision.

### Plan Review

For complex plans, optionally run sequential multi-delegate review:

- architect/reviewer checks soundness and blocking counterarguments
- critic/reason delegate evaluates completeness, testability, blast radius, and rollback
- verdicts are recorded in the plan IR and fed back into plan revision

### CLI and MCP

```bash
aimee clarify "vague task description"
aimee clarify status <id>
aimee clarify spec <id>
aimee plan review <plan_id>
```

MCP tools:

- `clarify_start`
- `clarify_answer`
- `review_plan`

### Changes

| File | Change |
|------|--------|
| `src/agent_clarify.c` | Clarification loop, scoring delegation, spec crystallization |
| `src/agent_plan.c` | Request classification, plan validation, and review integration |
| `src/headers/agent_clarify.h` | Clarification types and declarations |
| `src/headers/agent_plan.h` | Classification, validation, and review state/types |
| `src/cmd_core.c` | `clarify` and plan-review command routing |
| `src/mcp_tools.c` | Clarification and plan-review MCP tools |
| `src/db.c` | Clarification storage plus plan review metadata |

## Acceptance Criteria

- [ ] Vague tasks can enter a structured clarification flow with persisted ambiguity scoring.
- [ ] Trivial tasks can skip heavyweight planning based on fast classification.
- [ ] Plans with invalid file or symbol references are flagged before execution.
- [ ] Complex plans can be contested and revised through review before implementation.
- [ ] Clarified specs can feed directly into plan creation.
- [ ] Planning depth remains overrideable by the user.

## Owner and Effort

- **Owner:** aimee
- **Effort:** L
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Start with classification + clarification, then add validation, then add review gates for complex plans only.
- **Rollback:** Each stage can degrade independently to simpler planning behavior.
- **Blast radius:** Medium. This changes how and when plans are accepted for execution.

## Test Plan

- [ ] Unit tests: request classification and planning-depth overrides
- [ ] Unit tests: ambiguity scoring state transitions
- [ ] Unit tests: plan validation for missing files/symbols and vague tasks
- [ ] Unit tests: review iteration and verdict recording
- [ ] Integration tests: vague task → clarification → plan → validation → review

## Operational Impact

- **Metrics:** request complexity distribution, clarification rounds, plan validation failures, plan review verdicts
- **Logging:** classification, clarification readiness, validation warnings, review verdicts
- **Alerts:** None
- **Disk/CPU/Memory:** Extra delegate calls only for clarification scoring and complex plan review

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Classification + clarification | P1 | M | High |
| Plan validation | P1 | S | High |
| Plan review loop | P1 | M | High |

## Trade-offs

- **Why merge these planning proposals?** Clarification, depth selection, validation, and review are successive gates on the same planning artifact.
- **Why keep clarification advisory for concrete tasks?** For obvious small tasks, forcing a clarification loop would be overhead.
- **Why validate mechanically before delegate review?** Cheap reference validation catches the easiest failures first.
