# Proposal: <Title>

## Problem

What is broken, missing, or suboptimal? Include evidence (error logs, metrics, user reports, code references). Avoid solutioning here.

## Goals

- Bullet list of what success looks like, stated as outcomes, not tasks.

## Approach

Describe the design. Include code sketches, schema changes, or architecture diagrams where they clarify. Reference specific files and functions.

### Changes

| File | Change |
|------|--------|
| `src/example.c` | Add X to Y |

## Acceptance Criteria

Measurable conditions that define "done." Each criterion should be independently verifiable.

- [ ] Criterion 1 (e.g., `aimee foo --bar` returns exit 0 with output matching X)
- [ ] Criterion 2 (e.g., CI job `static-analysis` blocks PRs on new warnings)
- [ ] Criterion 3 (e.g., latency p99 < 50ms under benchmark Y)

## Owner and Effort

- **Owner:** <name or role>
- **Effort:** <T-shirt size: S/M/L/XL, or hours/days estimate>
- **Dependencies:** <other proposals, PRs, or external work that must land first>

## Rollout and Rollback

- **Rollout:** How is this deployed or enabled? (e.g., feature flag, migration, config change, CI gate)
- **Rollback:** How do we undo this if it breaks? (e.g., revert commit, run reverse migration, toggle flag off)
- **Blast radius:** What breaks if this goes wrong? (e.g., all sessions, only new sessions, only CI)

## Test Plan

- [ ] Unit tests: <what is covered>
- [ ] Integration tests: <what scenarios>
- [ ] Failure injection: <what failure modes are tested> (e.g., interrupted migration, disk full, concurrent access)
- [ ] Manual verification: <steps to confirm behavior>

## Operational Impact

- **Metrics:** New or changed metrics/counters.
- **Logging:** New log lines or changed log levels.
- **Alerts:** New alert conditions or changed thresholds.
- **Disk/CPU/Memory:** Expected resource impact.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| ... | P1/P2/P3 | S/M/L | ... |

## Trade-offs

What alternatives were considered and why were they rejected? What are the known limitations of this approach?
