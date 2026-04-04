# Proposal: Dashboard Windowed Layout with Logs Panel

## Problem

The dashboard renders each section (Delegations, Metrics, Traces, Memory, Plans) as full-width vertically stacked cards. Long tables push subsequent sections off-screen, requiring users to scroll through multiple screens to find the section they need. There is also no way to view recent activity logs from the dashboard.

## Goals

- All dashboard sections visible on a single screen in a windowed grid layout.
- Each section is independently scrollable.
- Add a Logs panel showing combined agent and decision activity.

## Approach

### 1. Windowed grid layout

Replace the vertically stacked `.grid` with a CSS Grid that fills the viewport: `grid-template-columns: 1fr 1fr 1fr; grid-template-rows: 1fr 1fr`. Each cell (`.win`) is a flex column with a fixed header and a scrollable body (`overflow-y: auto`). The body uses `overflow: hidden` on the parent and `min-height: 0` to ensure proper flex shrinking.

### 2. Layout: 3x2 grid

| Delegations | Metrics | Execution Plans |
|-------------|---------|-----------------|
| Traces      | Memory  | Logs            |

### 3. Logs API endpoint

New `/api/logs` endpoint returning a JSON array that unions `agent_log` and `decision_log` tables, ordered by timestamp descending, limited to 100 entries. Each entry has: `source` (agent/decision), `who`, `what`, `detail`, `timestamp`.

### 4. Logs panel rendering

The Logs window shows a table with columns: Time, Source (color-coded), Who, What, Detail (truncated with tooltip).

### 5. Parallel data loading

All six API calls (`/api/delegations`, `/api/metrics`, `/api/traces`, `/api/memory-stats`, `/api/plans`, `/api/logs`) are fetched with `Promise.all` for faster loading.

## Files changed

- `src/webchat.c` — rewrote `dashboard_page_html` with windowed layout, added `/api/logs` route
- `src/dashboard.c` — added `api_logs()` function
- `src/headers/dashboard.h` — declared `api_logs()`
