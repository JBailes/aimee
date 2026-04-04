# Proposal: Reduce Command Surface Area and Split Core from Experimental Features

## Problem

The command table in `src/cmd_table.c` exposes a broad and growing surface area: memory, hooks, agents, delegation, dashboard, webchat, work queues, jobs, plans, eval, import/export, DB diagnostics, Git/PR operations, branching, configuration, and more. This increases maintenance cost in three ways:

1. More commands means more parser/help/dispatch churn in already-large command files.
2. Features with very different stability expectations evolve together in the same release surface.
3. The codebase carries complexity for workflows that are adjacent to aimee's core value rather than central to it.

As a result, cleanup efforts in `cmd_core.c`, `cmd_agent.c`, and related files are fighting both implementation complexity and product-surface sprawl.

## Goals

- Define a smaller, clearer core command surface for aimee.
- Separate core workflows from experimental, niche, or auxiliary tooling.
- Reduce the number of places that need to change when adding or evolving a feature.
- Improve discoverability by making the top-level CLI less crowded.

## Approach

Introduce command tiers:

- **Core:** session startup, hooks, memory, rules, config, index, delegate
- **Advanced:** workspace, worktree, trace, jobs, plans, status
- **Experimental/Admin:** dashboard, webchat, eval, import/export, DB maintenance, branch orchestration, Git helpers

Then make the separation concrete:

- keep the core commands first-class in help and docs
- move advanced/admin commands behind clearer namespaces or feature gates where appropriate
- require a higher bar for new top-level commands

This is a product decision as much as a code-organization decision. It narrows the amount of behavior that must be maintained as one coherent CLI.

### Changes

| File | Change |
|------|--------|
| `src/cmd_table.c` | Group commands by tier and de-emphasize non-core entries in default help |
| `src/main.c` | Keep top-level usage focused on core workflows |
| `docs/COMMANDS.md` | Reorganize documentation by command tier |
| `docs/STATUS.md` | Mark advanced/experimental commands explicitly |
| `docs/COMPATIBILITY.md` | Document which command tiers are expected to be stable |
| `src/cmd_core.c` | Reduce top-level help/dispatch burden after command-tier cleanup |

## Acceptance Criteria

- [ ] The default `aimee` help output foregrounds only the core command set.
- [ ] Advanced and experimental/admin commands remain available but are clearly labeled.
- [ ] New top-level commands require an explicit justification in proposals or review.
- [ ] Documentation reflects the new tiers consistently.
- [ ] The reduced top-level surface lowers churn in core command dispatch/help code.

## Owner and Effort

- **Owner:** aimee product/core
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** First relabel/document command tiers, then optionally hide or namespace some advanced commands from default help output.
- **Rollback:** Restore current help/documentation ordering and visibility.
- **Blast radius:** CLI help, documentation, and user expectations for discoverability.

## Test Plan

- [ ] Unit tests: help output tests and command-table visibility tests.
- [ ] Integration tests: representative command invocations still resolve correctly after re-tiering.
- [ ] Failure injection: ensure hidden/de-emphasized commands still work when invoked directly.
- [ ] Manual verification: compare `aimee`, `aimee help`, and `docs/COMMANDS.md` before and after the reorganization.

## Operational Impact

- **Metrics:** Optionally track command usage by tier to inform future pruning.
- **Logging:** None required.
- **Alerts:** None.
- **Disk/CPU/Memory:** None.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Define core vs advanced command tiers | P2 | S | Reduces conceptual sprawl |
| Reorganize help/docs around tiers | P2 | S | Improves usability and lowers maintenance pressure |
| Gate new top-level commands | P1 | S | Prevents future surface-area creep |

## Trade-offs

Some users will prefer every feature to remain equally visible, but equal visibility is part of what created the current sprawl. An alternative is leaving the surface area intact and only refactoring internals, but that keeps product complexity on the same upward path even if the code is temporarily cleaner.
