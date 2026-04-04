# Proposal: Work Queue Idle-Claim Reclamation

## Problem

Because `aimee` is designed for a single user on a single machine, we are
removing strict session ownership of work items (see `work-queue-session-fix.md`).
This simplifies the workflow, but it means claims can sit in the queue indefinitely
if the user or an agent abandons them without explicitly calling `work release` or
`work fail`.

There is no explicit `work gc` command to clear out these old claims, and
`work list` does not surface claim age to help the user identify them.

## Goals

- Reclaim obviously idle claims before they block the queue for too long
- Add an explicit `aimee work gc` command
- Surface `claimed_at` in list output

## Approach

### 1. Add time-based idle reclaim

During `work claim` and `work gc`, automatically release claims older than a
configurable timeout (e.g., 2 hours). Since items are no longer tied to
sessions, age is the only metric needed to determine if a claim was abandoned.

### 2. Add `aimee work gc`

Provide an explicit cleanup command that reports which items were released
back to the pending state due to age.

### 3. Show claim age in `work list`

Include `claimed_at` or a relative age so operators can see how long an item
has been claimed.

## Changes

| File | Change |
|------|--------|
| `src/cmd_work.c` | Add idle-claim cleanup path, `work gc`, and claim-age output |

## Acceptance Criteria

- [ ] Idle claims older than the timeout can be reclaimed automatically or via gc
- [ ] `aimee work gc` reports released items
- [ ] `aimee work list` shows claim age or claim timestamp for claimed items

