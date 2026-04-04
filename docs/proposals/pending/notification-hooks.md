# Proposal: Event Notification Hooks

## Problem

When aimee runs long-running operations — background delegations, completion loops, pipeline phases, or jobs — the user has no way to be notified of completion or failure unless they actively poll. This is particularly painful for:
- Background delegations (`--background`) that may take minutes
- Pipeline phases that run autonomously
- Verification failures that need user attention
- Circuit breaker triggers that halt automated loops

The user must either watch the terminal or remember to check back.

oh-my-codex's notification system routes events to Discord, Telegram, Slack, and custom webhooks with configurable verbosity. While aimee doesn't need full multi-platform notification, the core idea — fire-and-forget event notifications — would improve the UX of async workflows.

Evidence:
- `--background` delegation returns a task ID but there's no notification when it completes
- Pipeline and completion loop proposals would benefit from notifications at phase transitions
- The web dashboard shows historical data but doesn't push notifications
- No webhook/notification infrastructure exists in aimee

There is also a duplicate proposal focused only on desktop notifications for idle sessions. Desktop notifications should be one built-in target of the broader event-hook system, not a standalone feature.

## Goals

- Configurable event hooks fire notifications when key events occur
- Notification targets are extensible: webhook, shell command, or desktop notification
- Verbosity is configurable — users choose which events they care about
- Notifications are fire-and-forget — they don't block the workflow

## Approach

### 1. Event types

Define events in `headers/events.h`:

```c
typedef enum {
    EVENT_DELEGATE_COMPLETE,    // background delegation finished
    EVENT_DELEGATE_FAILED,      // delegation error
    EVENT_VERIFY_PASS,          // verification gate passed
    EVENT_VERIFY_FAIL,          // verification gate failed
    EVENT_PLAN_REVIEW_DONE,     // consensus review completed
    EVENT_PIPELINE_PHASE,       // pipeline phase transition
    EVENT_CIRCUIT_BREAK,        // completion/QA loop circuit breaker triggered
    EVENT_JOB_COMPLETE,         // coordinated job finished
    EVENT_SESSION_IDLE,         // session idle for N minutes
} aimee_event_t;
```

### 2. Notification targets

Configuration in `~/.config/aimee/notifications.json`:

```json
{
  "enabled": true,
  "verbosity": "standard",
  "targets": [
    {
      "type": "command",
      "command": "notify-send 'aimee' '{{message}}'"
    },
    {
      "type": "webhook",
      "url": "https://hooks.slack.com/...",
      "events": ["circuit_break", "verify_fail", "pipeline_phase"]
    }
  ],
  "verbosity_levels": {
    "minimal": ["circuit_break", "job_complete"],
    "standard": ["delegate_complete", "verify_fail", "pipeline_phase", "circuit_break", "job_complete"],
    "verbose": ["*"]
  }
}
```

### 3. Fire-and-forget dispatch

```c
void event_notify(app_ctx_t *ctx, aimee_event_t event, const char *message);
```

This function:
1. Checks if notifications are enabled and the event matches the verbosity level
2. For each target that matches the event: fork and exec the command or POST to the webhook
3. Fire-and-forget — don't wait for the notification to complete
4. Log failures to stderr but don't error

### 4. Built-in desktop notification

For Linux/macOS, a default target using `notify-send` (Linux) or `osascript` (macOS) provides zero-config desktop notifications:

```bash
aimee notify on               # enable with default desktop notifications
aimee notify off              # disable
aimee notify --verbose        # all events
aimee notify test             # send a test notification
```

### Changes

| File | Change |
|------|--------|
| `src/events.c` | New: event dispatch, notification targets, fire-and-forget delivery |
| `src/headers/events.h` | New: event types, notification function |
| `src/config.c` | Load notification config |
| `src/cmd_core.c` | Add `notify` subcommand |
| `src/tests/test_events.c` | Tests for event dispatch, verbosity filtering |

## Acceptance Criteria

- [ ] `aimee notify on` enables desktop notifications with zero config
- [ ] Background delegation completion triggers a notification
- [ ] Idle session states such as "needs input", "permission needed", and "stalled" can trigger desktop notifications with debounce
- [ ] Circuit breaker triggers generate a notification
- [ ] Webhook targets receive POST with event data
- [ ] Custom command targets execute with template variable substitution
- [ ] Notifications are fire-and-forget — never block the workflow
- [ ] Verbosity filtering works — minimal/standard/verbose

## Owner and Effort

- **Owner:** aimee
- **Effort:** S (2-3 focused sessions)
- **Dependencies:** None, but becomes more valuable with pipeline/completion-loop/job proposals

## Rollout and Rollback

- **Rollout:** Disabled by default. `aimee notify on` to enable.
- **Rollback:** Revert commit. Notifications simply stop.
- **Blast radius:** None — fire-and-forget, no workflow impact.

## Test Plan

- [ ] Unit tests: event dispatch with various verbosity levels
- [ ] Unit tests: target matching — command vs webhook, event filters
- [ ] Unit tests: template variable substitution
- [ ] Integration tests: background delegation → notification fires
- [ ] Manual verification: enable notifications, run a delegation, observe desktop notification

## Operational Impact

- **Metrics:** `notifications_sent{event,target_type}`, `notifications_failed`
- **Logging:** Failed notifications to stderr: `aimee: notify: webhook POST failed (timeout)`
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible. Fork+exec per notification.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Event types + dispatch | P1 | S | High — core mechanism |
| Desktop notification target | P1 | S | High — zero-config value |
| Webhook target | P2 | S | Medium — integration |
| Custom command target | P2 | S | Medium — extensibility |
| Verbosity configuration | P3 | S | Low — refinement |

## Trade-offs

**Why fire-and-forget instead of delivery guarantees?**
Notifications are informational, not transactional. If a desktop notification is missed, the user can check status via CLI or HUD. Adding retry/queue infrastructure for notifications is overkill.

**Why not integrate with the existing dashboard?**
The dashboard is pull-based (HTTP polling). Notifications are push-based. They complement each other — the dashboard provides detail, notifications provide alerts.

**Why a config file instead of DB-stored config?**
Notification config is per-machine (different machines have different desktop environments, webhook URLs). A dotfile is the standard place for per-machine configuration in CLI tools.
