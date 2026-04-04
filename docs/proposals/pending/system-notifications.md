# Proposal: System Notifications for Idle Sessions

## Problem

When Aimee completes a task or needs human input, there's no notification — the user must actively check. For long-running delegates, this means the user either polls repeatedly or misses completion for minutes/hours. This is especially painful when the agent needs permission or has a question that blocks further progress.

Evidence: oh-my-openagent implements session notifications (`src/hooks/session-notification.ts`) that detect idle sessions (waiting for input, asking questions, needing permissions) and send desktop notifications via platform-appropriate mechanisms. It includes a configurable idle confirmation delay to avoid false positives.

## Goals

- Notify the user when a session becomes idle (completed, waiting for input, or stalled)
- Support Linux desktop notifications (`notify-send`) as the primary channel
- Configurable notification content and delay
- Avoid spam — don't notify for brief pauses during active work

## Approach

Add notification dispatch to `platform_event.c`. When a session transitions to idle state (no tool calls for N seconds, and either completed or waiting for user input), fire a notification.

### Notification triggers

| State | Notification |
|-------|-------------|
| Task completed | "Aimee: task complete" |
| Waiting for user input | "Aimee: needs your input" |
| Permission required | "Aimee: permission needed" |
| Delegate stalled | "Aimee: delegate stalled on <task>" |

### Changes

| File | Change |
|------|--------|
| `src/platform_event.c` | Add `platform_notify()` using `notify-send` on Linux |
| `src/agent_eval.c` | Detect idle transitions and call notification |
| `src/config.c` | Parse notification config (delay, channels, enabled) |

## Acceptance Criteria

- [ ] Completed sessions trigger a desktop notification
- [ ] Notification includes session context (task name or summary)
- [ ] Idle confirmation delay prevents false positives (default: 3s)
- [ ] Notifications are disabled by default, enabled via config
- [ ] Missing `notify-send` binary logs a warning but doesn't error

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (< 1 day)
- **Dependencies:** `notify-send` available on the system (libnotify)

## Rollout and Rollback

- **Rollout:** Config-driven; disabled by default
- **Rollback:** Disable in config; no notifications sent
- **Blast radius:** External to Aimee — only affects desktop notification bus

## Test Plan

- [ ] Unit test: idle transition triggers notification call
- [ ] Unit test: brief pause (<delay) does not trigger
- [ ] Unit test: missing notify-send binary handled gracefully
- [ ] Manual test: complete a task, verify desktop notification appears

## Operational Impact

- **Metrics:** Count of notifications sent per session
- **Logging:** Log notification dispatch at debug level
- **Disk/CPU/Memory:** One process spawn per notification; negligible

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| System Notifications | P2 | S | Low-Medium — quality of life improvement |

## Trade-offs

Alternative: webhook notifications (HTTP POST to a URL). More flexible but more complex to configure. Desktop notifications cover the primary use case (single-user homelab). Webhook support can be added later.

Inspiration: oh-my-openagent `src/hooks/session-notification.ts`
