# Proposal: Session HUD — Real-Time Workflow Status Display

## Problem

When aimee runs multi-step workflows (planning, delegation, verification, completion loops), the user has no real-time visibility into what's happening. Status is only available by running explicit commands (`aimee plans list`, `aimee job status`). During long-running operations, the user is blind.

Aimee has a web dashboard (`dashboard.c` on port 9200) that shows delegations, metrics, traces, and plans, but:
- It's a pull-based web UI — you have to open a browser and refresh
- It doesn't show live workflow state (which phase of a pipeline, which iteration of a completion loop)
- No terminal-based option for users who prefer CLI

oh-my-codex's HUD provides a terminal-based live status display with `--watch` mode that shows current mode, iteration counts, worker status, and timing — all from file-based state polling. The insight is that a lightweight terminal HUD is more useful during active development than a full web dashboard.

Evidence:
- `dashboard.c` serves an HTML page on :9200 — great for overview, but not for live monitoring during work
- No terminal-based status display exists
- No way to see pipeline/completion-loop/job progress without running a command
- The web dashboard doesn't show phase-level workflow state

## Goals

- A terminal-based HUD shows live workflow state (current mode, active plan, job progress, delegate activity)
- The HUD updates automatically (polling-based, like `watch`)
- Works alongside the primary agent session without interfering
- Lightweight — no dependencies beyond terminal capabilities

## Approach

### 1. `aimee hud` command

```bash
aimee hud              # one-shot status snapshot
aimee hud --watch      # live updating (1s interval)
aimee hud --watch 5    # live updating (5s interval)
```

### 2. Status data sources

The HUD aggregates state from existing aimee subsystems:

| Section | Source | What it shows |
|---------|--------|--------------|
| Mode | Config/session state | plan/implement/ecomode |
| Plan | `execution_plans` table | Active plan, step progress (3/7 done) |
| Job | `jobs` + `job_tasks` tables | Running job, task status counts |
| Pipeline | `pipelines` table | Current phase, iteration count |
| Completion | Plan completion state | Iteration N/max, last error |
| Delegates | Recent `delegate_attempts` | Active/recent delegations, latency |
| Verify | Last verify run | Pass/fail, finding counts |
| Session | Session metadata | Duration, provider, token estimate |

### 3. Terminal rendering

Use ANSI escape codes for a compact, colored display:

```
━━━ aimee hud ━━━━━━━━━━━━━━━━━━━━━━━━━
 mode: implement │ eco: off │ session: 23m
 plan: #12 [executing] 4/7 steps done
 job:  #3  [running] 2 active, 1 pending
 pipe: #1  [qa] iter 2/5 (build: FAIL)
 delegates: 2 active (local-ollama, claude)
 verify: PASS (0 critical, 2 low findings)
 last: delegate [review] → ok (1.2s ago)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

Implementation:
- Query each data source (existing DB queries + session state files)
- Render with ANSI colors (green=ok, yellow=active, red=fail)
- `--watch` mode: clear screen, redraw every N seconds
- Output to stderr if stdout is piped (so it can coexist with other tools)

### 4. JSON output mode

```bash
aimee hud --json       # machine-readable status
```

Returns the same data as JSON, enabling integration with external monitoring or custom dashboards.

### 5. Integration with existing dashboard

Add a `/api/hud` endpoint to `dashboard.c` that returns the same aggregated status as JSON. The web dashboard can then include a live status section alongside its existing cards.

### Changes

| File | Change |
|------|--------|
| `src/cmd_wm.c` or new `src/hud.c` | HUD rendering, data aggregation, watch loop |
| `src/dashboard.c` | Add `/api/hud` endpoint |
| `src/cmd_core.c` | Add `hud` subcommand routing |
| `src/tests/test_hud.c` | Tests for data aggregation and JSON output |

## Acceptance Criteria

- [ ] `aimee hud` shows a one-shot status snapshot in the terminal
- [ ] `aimee hud --watch` updates live at configurable intervals
- [ ] Status includes: mode, active plan/job/pipeline, delegate activity, verify status
- [ ] `aimee hud --json` outputs machine-readable status
- [ ] `/api/hud` endpoint added to web dashboard
- [ ] HUD handles missing state gracefully (no active plan → section omitted)

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (2-3 focused sessions)
- **Dependencies:** None required, but shows richer data when other proposals (pipeline, jobs, completion loop) are implemented

## Rollout and Rollback

- **Rollout:** New command. No changes to existing behavior.
- **Rollback:** Revert commit. No impact.
- **Blast radius:** None — read-only command.

## Test Plan

- [ ] Unit tests: data aggregation from each source
- [ ] Unit tests: JSON output format
- [ ] Unit tests: graceful handling of missing data (no active plan, no delegates)
- [ ] Manual verification: run HUD alongside a delegation, observe live status

## Operational Impact

- **Metrics:** None (read-only feature)
- **Logging:** None
- **Alerts:** None
- **Disk/CPU/Memory:** DB queries every 1-5 seconds in watch mode. Negligible.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Core HUD rendering + data aggregation | P1 | S | High — immediate visibility |
| Watch mode | P1 | S | High — live monitoring |
| JSON output | P2 | S | Medium — programmability |
| Dashboard API integration | P3 | S | Low — web dashboard already exists |

## Trade-offs

**Why terminal-based instead of improving the web dashboard?**
The web dashboard requires opening a browser. A terminal HUD can run in a tmux pane next to the agent session — lower friction for the common case. Both can coexist.

**Why polling instead of push-based updates?**
Push would require a notification mechanism (websockets, IPC). Polling every 1-5 seconds from SQLite is negligible overhead and dramatically simpler. The HUD is read-only; there's no state to keep in sync.

**Why not embed the HUD in the agent session output?**
The agent controls its own output format. A separate HUD command runs in its own terminal/pane, avoiding interference with the agent's conversation flow.
