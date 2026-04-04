# Proposal: Lean Refactor Audit

## Problem

Aimee has accumulated proposal churn, overlapping ideas, and implementation drift that can encourage broad refactors without a clear inventory of what is actually worth simplifying. That leads to noisy cleanup work, duplicate proposals, and audits that optimize for volume instead of impact.

The actual need is a lean refactor audit: identify concentrated areas of duplication, stale scaffolding, and over-engineered paths, then turn that audit into a narrow queue of high-value refactors.

## Goals

- Audit the codebase and proposal surface for refactor candidates with a bias toward high signal.
- Prefer deletion, consolidation, and simplification over speculative redesign.
- Distinguish proven pain points from aesthetic cleanup.
- Produce an actionable shortlist rather than an open-ended modernization effort.
- Keep audit output cheap to update and easy to verify.

## Approach

Create a lightweight audit document and workflow that favors evidence over ambition.

### Audit Heuristics

For each candidate area, capture:

- duplicated logic or overlapping proposals
- dead paths, stale scaffolding, or obsolete compatibility code
- pass-through abstractions with little policy value
- churn hotspots where complexity keeps reappearing
- verification cost and expected simplification payoff

### Output Format

Each audit entry should include:

- scope
- observed issue
- why it matters
- recommended lean action
- expected risk
- verification plan

### Changes

| File | Change |
|------|--------|
| `docs/proposals/lean-refactor-audit.md` | Add proposal describing a lean, evidence-driven refactor audit |

## Acceptance Criteria

- [ ] Audit guidance emphasizes narrow, evidence-backed refactors.
- [ ] Output format makes deletion/consolidation opportunities explicit.
- [ ] Audit entries include risk and verification expectations.
- [ ] Proposal is scoped so it can guide future cleanup without forcing immediate code changes.

## Owner and Effort

- **Owner:** aimee
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Use the audit as a planning artifact before opening refactor PRs.
- **Rollback:** Remove or replace the proposal if it creates bureaucracy instead of sharper prioritization.
- **Blast radius:** Documentation only.

## Test Plan

- [ ] Review against existing proposals to confirm it reduces overlap rather than adding a competing process.
- [ ] Validate that future refactor ideas can be expressed in the audit format with concrete verification steps.
