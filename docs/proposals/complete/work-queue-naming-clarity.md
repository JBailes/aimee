# Proposal: Rename `aimee queue` to Avoid Confusion with `aimee work`

## Problem

`aimee queue` and `aimee work` are two completely different systems that
share a confusing naming overlap:

- **`aimee queue`** (in `cmd_agent_trace.c`): fires N tasks at configured
  agents *immediately* and synchronously. Each task runs a prompt against an
  agent and returns the result. There is no persistence, no claim semantics,
  no queue. It is batch parallel execution.

- **`aimee work`** (in `cmd_work.c`): a persistent work queue in SQLite
  with claim/complete/fail semantics for coordinating across sessions.

During initial queue setup, I ran `aimee queue` with 36 JSON tasks expecting
them to be added to a persistent queue. Instead, all 36 fired immediately
against the configured agents (which had no "execute" role, so all failed).
This wasted time and produced confusing error output.

The word "queue" in `aimee queue` is misleading because it implies
persistence and ordering, when it actually means "dispatch these tasks right
now in parallel."

## Goals

- The two systems have clearly distinct names.
- Users do not accidentally fire immediate execution when they want
  persistent queueing.

## Approach

Rename `aimee queue` to `aimee dispatch` (or `aimee run-batch`):

```c
/* In cmd_table.c or wherever commands are registered */
{"dispatch", "Run multiple tasks in parallel via agents", cmd_queue},
```

Keep `aimee queue` as a hidden alias for backward compatibility, but
deprecation-warn:

```
aimee: 'queue' is deprecated, use 'dispatch' instead
```

### Changes

| File | Change |
|------|--------|
| `src/cmd_table.c` | Register `dispatch` as primary, `queue` as deprecated alias |
| `src/cmd_agent_trace.c` | Add deprecation warning in `cmd_queue()` when invoked as "queue" |

## Acceptance Criteria

- [ ] `aimee dispatch` runs parallel tasks (same as current `aimee queue`)
- [ ] `aimee queue` still works but prints deprecation warning
- [ ] `aimee work` remains unchanged
- [ ] Help text clearly distinguishes dispatch (immediate) from work (persistent)

## Owner and Effort

- **Owner:** TBD
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Direct rename with alias.
- **Rollback:** git revert.
- **Blast radius:** Scripts using `aimee queue` continue to work (alias).

## Test Plan

- [ ] Manual: `aimee dispatch` works identically to old `aimee queue`
- [ ] Manual: `aimee queue` prints deprecation warning
- [ ] Manual: `aimee help` shows `dispatch` not `queue`

## Operational Impact

- **Metrics:** None.
- **Logging:** Deprecation warning on stderr.
- **Alerts:** None.
- **Disk/CPU/Memory:** None.

## Priority

P3. Naming clarity. Not functionally blocking.

## Trade-offs

Any rename has a cost: existing documentation, muscle memory, scripts. The
alias preserves backward compatibility. The benefit is eliminating a genuine
confusion point that wastes user time.
