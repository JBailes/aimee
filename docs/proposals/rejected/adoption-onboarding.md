# Proposal: Adoption and Onboarding Materials

> **Status: Pending.** Previously deferred pending on-demand server — that
> proposal is now done. Ready for implementation.

## Problem

The technical foundation is strong but there are no example repos, integration
templates, or getting-started guides that would help others adopt aimee. "0 stars,
0 forks, 0 issues" reflects missing onboarding materials, not missing quality.

## Goals

- A new user can go from `git clone` to a working session in under 5 minutes.
- Integration configurations are documented and derived from the canonical source
  (install.sh), not maintained separately.
- Major design decisions are documented as ADRs for contributors and evaluators.

## Approach

### 1. In-Repo Examples

Add an `examples/` directory in the main repo (not a separate repository) with:

- `examples/workspace.yaml` — sample workspace manifest for a multi-repo project
- `examples/rules.md` — sample rules showing the format
- `examples/facts.sh` — script that stores sample facts demonstrating the memory
  system
- `examples/hooks/` — reference hook configurations (see section 2)

Rationale: A separate `aimee-example` repo drifts unless tested in CI. In-repo
examples can be validated by the existing build. If a standalone starter repo is
needed later, it can be generated from these examples.

### 2. Integration Templates (Derived from install.sh)

Extract reference configurations from `install.sh` (the canonical source for hook
registration) into documentation files:

- `examples/hooks/claude-settings.json` — Claude Code hook configuration
- `examples/hooks/gemini-settings.json` — Gemini CLI hook configuration
- `examples/hooks/codex-hooks.json` — Codex CLI hook configuration

These are **reference copies** for users who want to understand or customize the
integration. The install script remains the source of truth. Each file includes a
header comment: `/* Generated from install.sh — do not edit directly */`.

CI validation: Add a test that extracts the matcher patterns from `install.sh`
and verifies they match the example files. This catches drift.

### 3. Getting Started Guide

`docs/GETTING_STARTED.md` — step-by-step tutorial:

1. Install aimee (build from source: 3 commands)
2. Run `aimee init` (creates database and config)
3. Run `aimee setup` (provisions from workspace manifest)
4. Store a fact: `aimee memory store --tier L2 --kind fact my-fact "example value"`
5. Verify it persists: `aimee memory search "example"`
6. Start a session: `aimee` (launches provider with worktree isolation)
7. Delegate a task: `aimee delegate execute "echo hello"`

Each step includes expected output so the user can verify success.

### 4. Architecture Decision Records

`docs/adr/` directory using the Nygard format (Context, Decision, Status,
Consequences):

- `docs/adr/TEMPLATE.md` — ADR template defining the required sections
- `docs/adr/001-pure-c.md` — why C instead of Go/Rust/Python
- `docs/adr/002-sqlite-single-file.md` — why SQLite over Postgres/Redis
- `docs/adr/003-hook-intercept-model.md` — why pre/post hooks instead of proxy
- `docs/adr/004-4-tier-memory.md` — why 4 tiers instead of flat key-value
- `docs/adr/005-binary-split.md` — why thin client + server instead of monolith
- `docs/adr/006-worktree-isolation.md` — why git worktrees for session isolation

Each ADR follows the template: Title, Date, Status (accepted), Context (what
problem we faced), Decision (what we chose), Consequences (trade-offs).

### Changes

| Deliverable | Location |
|-------------|----------|
| In-repo examples | `examples/workspace.yaml`, `examples/rules.md`, `examples/facts.sh` |
| Hook reference configs | `examples/hooks/claude-settings.json`, `examples/hooks/gemini-settings.json`, `examples/hooks/codex-hooks.json` |
| Getting started guide | `docs/GETTING_STARTED.md` |
| ADR template | `docs/adr/TEMPLATE.md` |
| ADRs | `docs/adr/001-pure-c.md` through `006-worktree-isolation.md` |
| CI drift check | Test that verifies example hook configs match install.sh |

## Acceptance Criteria

- [ ] `examples/` directory contains working workspace manifest, rules, and facts
      script
- [ ] Hook reference configs exist for Claude, Gemini, and Codex
- [ ] CI test verifies hook example matcher patterns match install.sh patterns
- [ ] `docs/GETTING_STARTED.md` covers install through first delegation with
      expected output at each step
- [ ] Following the guide on a clean machine results in a working session within
      5 minutes
- [ ] `docs/adr/TEMPLATE.md` defines the ADR format
- [ ] 6 ADRs exist covering the major design decisions
- [ ] All ADRs follow the template (Context, Decision, Status, Consequences)

## Owner and Effort

- **Owner:** TBD
- **Effort:** M (documentation-heavy, small CI test)
- **Dependencies:** None — can be done at any time

## Rollout and Rollback

- **Rollout:** Merge documentation and examples. No runtime changes.
- **Rollback:** Revert commit. No operational impact.
- **Blast radius:** None — documentation and examples only.

## Test Plan

- [ ] CI: hook example files match install.sh patterns (automated drift check)
- [ ] Manual: follow GETTING_STARTED.md on a clean Ubuntu machine, time the
      process, verify all steps produce expected output
- [ ] Manual: review each ADR for completeness (all template sections filled)
- [ ] Manual: run `examples/facts.sh` and verify facts are stored and searchable

## Operational Impact

- **Metrics:** None.
- **Logging:** None.
- **Alerts:** None.
- **Disk/CPU/Memory:** None.

## Priority

P2 — important for adoption but not blocking for existing users. Should wait
until operational documentation (runbooks) is stable so the getting-started guide
doesn't reference incomplete operational procedures.

## Trade-offs

**Why in-repo examples instead of a separate starter repo?** Separate repos drift
without dedicated CI. In-repo examples are validated by the existing build and
stay in sync with the codebase. A standalone starter repo can be generated later
if needed.

**Why derive templates from install.sh instead of maintaining separately?** The
PreToolUse matcher gap (PR #99) showed that install.sh is the source of truth for
hook configuration. Maintaining templates separately creates two sources of truth
that diverge silently.

**Why the Nygard ADR format?** It's the most widely adopted standard. Lightweight
enough to write quickly, structured enough to be useful. Alternatives (MADR, Y-
statements) add complexity without clear benefit for this project's scale.
