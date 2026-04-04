# Proposal: Decompose Large Modules as a Coordinated Roadmap

## Problem

aimee's maintainability problems are concentrated in a small number of oversized, multi-responsibility modules. Current hotspots include:

- `src/webchat.c` at 2622 lines
- `src/cmd_core.c` at 2280 lines
- `src/cmd_agent.c` at 1915 lines
- `src/agent_tools.c` at 1809 lines
- `src/guardrails.c` at 1687 lines
- `src/cmd_agent_trace.c` at 1683 lines
- `src/mcp_server.c` at 1581 lines
- `src/memory.c` at 1558 lines
- `src/agent.c` at 1540 lines
- `src/db.c` at 1493 lines

The repo already has relevant proposal history:

- `docs/proposals/pending/improve-module-boundaries.md`
- `docs/proposals/pending/refactor-cmd-core.md`
- rejected narrowly mechanical split proposals for `webchat`, `cmd_agent`, and `db/memory`

What is still missing is a sequencing proposal that turns those isolated ideas into one roadmap with dependencies, target boundaries, and stop conditions. Without that, the codebase accumulates more partial splits than durable module decomposition.

## Goals

- Break the worst hotspots into cohesive modules using responsibility boundaries, not arbitrary helper dumping.
- Sequence the work so shared abstractions land before file moves.
- Reuse and refine existing proposal work instead of reopening the same debate in piecemeal form.
- Bring the largest modules down to sizes that are reviewable and easier to own.

## Approach

Organize the decomposition into four waves.

### Wave 1: Shared abstractions and dispatch cleanup

Land low-risk enabling changes first:

- dispatch tables instead of long conditional chains
- shared path-policy and helper extraction
- focused headers where interfaces are too broad

This should build directly on `improve-module-boundaries.md`.

### Wave 2: Command-module splits

Refactor command-heavy monoliths around stable responsibilities:

- `cmd_core.c`: split into session/setup/config/status versus data/admin helpers
- `cmd_agent.c` and `cmd_agent_trace.c`: split provider setup/auth, execution/delegation, and job/trace inspection

This should absorb and replace the current narrow `refactor-cmd-core.md` proposal.

### Wave 3: Service/UI splits

Refactor the largest service-facing modules:

- `webchat.c`: separate transport, auth/session, request routing, streaming/provider glue, and embedded assets
- `mcp_server.c`: separate transport/protocol handling from tool registration/dispatch

### Wave 4: Data-layer decomposition

Reduce high-coupling data modules once command and service churn is lower:

- `db.c`: connection/bootstrap, migrations, and backup/recovery boundaries
- `memory.c` family: split by lifecycle, search, context assembly, and graph/link responsibilities where interfaces are too broad

### Changes

| File | Change |
|------|--------|
| `docs/proposals/pending/improve-module-boundaries.md` | Treat as an enabling dependency and merge overlap into this roadmap |
| `docs/proposals/pending/refactor-cmd-core.md` | Supersede with the broader command-wave plan |
| `src/cmd_core.c` | Split by domain responsibilities |
| `src/cmd_agent.c` | Split provider management from execution/auth responsibilities |
| `src/cmd_agent_trace.c` | Split delegation flow from jobs/trace/manifest inspection |
| `src/webchat.c` | Split server loop, auth/session, routing, streaming, and embedded assets |
| `src/mcp_server.c` | Split protocol transport from tool registry/handlers |
| `src/db.c` | Split bootstrap, migrations, and recovery/backup concerns |
| `src/memory*.c` | Continue header/API narrowing and split overly broad responsibilities where needed |
| `src/Makefile` | Add/refine build targets for newly extracted modules |

## Acceptance Criteria

- [ ] A documented wave plan exists with per-wave dependencies and target modules.
- [ ] The roadmap explicitly references and resolves overlap with existing proposals.
- [ ] Each wave can land independently without requiring one mega-refactor PR.
- [ ] Hotspot modules are reduced by responsibility, with no extracted file becoming a new junk drawer.
- [ ] The resulting module map is reflected in `src/README.md`.

## Owner and Effort

- **Owner:** aimee core
- **Effort:** XL
- **Dependencies:** `improve-module-boundaries.md`, structural guardrails, and ownership tracking

## Rollout and Rollback

- **Rollout:** Land one wave at a time, with each wave ending in a stable build/test point before the next starts.
- **Rollback:** Revert the affected wave or sub-wave; do not batch unrelated splits into one unrevertable change.
- **Blast radius:** Broad. Commands, server/UI, MCP, and data-layer internals are all touched across the full roadmap.

## Test Plan

- [ ] Unit tests: targeted coverage for each split module before and after extraction.
- [ ] Integration tests: command dispatch, session start, MCP server behavior, webchat flows, memory search, DB bootstrap.
- [ ] Failure injection: partial migration state, interrupted session setup, malformed MCP requests, invalid webchat auth/session state.
- [ ] Manual verification: hotspot files shrink and responsibilities become easier to map in `src/README.md`.

## Operational Impact

- **Metrics:** Track hotspot count, largest-file size, and module count per wave.
- **Logging:** None beyond existing logs unless new dispatch layers need diagnostics.
- **Alerts:** None.
- **Disk/CPU/Memory:** Negligible runtime effect expected; primary impact is maintainability and mergeability.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Wave 1 enabling abstractions | P1 | M | Unblocks safer module decomposition |
| Wave 2 command splits | P1 | L | Reduces hotspot churn in CLI-heavy areas |
| Wave 3 service/UI splits | P2 | L | Makes webchat and MCP changes safer |
| Wave 4 data-layer decomposition | P2 | L | Improves long-term maintainability in core storage paths |

## Trade-offs

This roadmap is intentionally broader than the previously rejected one-file split proposals. The trade-off is that it requires stronger sequencing discipline and may expose overlap with in-flight work. The alternative is continuing with isolated, file-specific split proposals, which has already led to repeated debate without a durable, repo-wide decomposition plan.
