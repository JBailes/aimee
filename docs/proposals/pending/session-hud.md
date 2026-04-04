# Proposal: Real-Time Session Status and HUD

## Problem

There are two overlapping status proposals:

- a terminal HUD for live workflow state
- in-session live stats for turns, tokens, tool outcomes, and context usage

These should be one observability proposal with multiple display surfaces instead of separate proposals with overlapping counters and APIs.

## Goals

- Expose real-time session and workflow status through CLI, chat, webchat, and machine-readable JSON.
- Show both workflow-level state and per-session stats.
- Keep the HUD lightweight and polling-based.
- Reuse one underlying status aggregation layer.

## Approach

Build one status aggregation layer and expose it through:

1. `aimee hud`
2. chat `/status`
3. webchat stats panel and API
4. JSON endpoints for other tools

### `aimee hud`

```bash
aimee hud
aimee hud --watch
aimee hud --json
```

### Aggregated State

The shared status layer should include:

- mode and provider
- active plan/job/pipeline/completion-loop state
- delegate activity
- verify status
- turn count
- token/context usage
- tool success/failure/skip counts

### In-Session `/status`

Add `/status` to CLI chat and a webchat stats panel using the same aggregated state.

### Dashboard and JSON

Expose the same status payload via:

- `/api/hud`
- session stats API
- JSON output for scripts and dashboards

### Changes

| File | Change |
|------|--------|
| `src/hud.c` or equivalent | Shared status aggregation, HUD rendering, watch loop |
| `src/agent.c` | Maintain turn counts and session stats |
| `src/agent_tools.c` | Maintain tool success/failure/skip counters |
| `src/cmd_chat.c` | Add `/status` using shared aggregation |
| `src/webchat.c` | Add stats API/SSE updates and wire shared aggregation |
| `src/webchat_assets.c` | Add stats panel/sidebar |
| `src/dashboard.c` | Add `/api/hud` endpoint |
| `src/cmd_core.c` | Add `hud` subcommand |

## Acceptance Criteria

- [ ] `aimee hud` shows a one-shot status snapshot in the terminal.
- [ ] `aimee hud --watch` updates live at configurable intervals.
- [ ] Status includes workflow state plus token/context/tool counters.
- [ ] `/status` in chat exposes the same session stats without leaving the session.
- [ ] JSON endpoints expose the same aggregated status for automation.
- [ ] Missing state is handled gracefully.

## Owner and Effort

- **Owner:** aimee
- **Effort:** M
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Start with shared aggregation and `aimee hud`, then add chat/webchat surfaces.
- **Rollback:** Remove display surfaces independently; underlying counters remain useful elsewhere.
- **Blast radius:** Display only.

## Test Plan

- [ ] Unit tests: data aggregation from each source
- [ ] Unit tests: JSON output format
- [ ] Integration tests: live session updates reflected in HUD and `/status`
- [ ] Manual verification: run HUD alongside an active delegation

## Operational Impact

- **Metrics:** None beyond existing counters
- **Logging:** None required beyond debug during development
- **Alerts:** None
- **Disk/CPU/Memory:** Polling overhead only

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Shared status aggregation | P1 | S | High |
| HUD + watch mode | P1 | S | High |
| Chat/webchat status surfaces | P2 | S | Medium |

## Trade-offs

- **Why merge HUD and live-session-stats?** They depend on the same counters and aggregation logic.
- **Why keep polling?** It is simpler and adequate for read-only monitoring.
- **Why not only improve the web dashboard?** A terminal HUD is lower-friction during active development.
