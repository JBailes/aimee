# Proposal: Time-Based Rule Weight Decay

## Problem

Rule weights in the rules table (`rules.c`, `feedback.c`) only increase via `feedback_record()` (+50 per reinforcement, capped at 100) and `anti_pattern_escalate()` (set to 90). There is no decay mechanism. A rule created from a one-time incident months ago retains full weight forever. Over time, the rule set grows monotonically and dilutes signal. The memory system has demotion and expiry (`memory_promote.c`), but rules have no equivalent.

## Goals

- Rule weights decay over time if not reinforced
- Stale rules eventually archive (weight drops below threshold)
- Frequently reinforced rules remain strong
- Decay rate is configurable and kind-aware (hard directives decay slower)

## Approach

1. Add a `last_reinforced_at` timestamp column to the rules table (migration in `db.c`). Set to `updated_at` for existing rows.
2. During session cleanup in `cmd_hooks.c` (where `eval_feedback_loop` runs), call a new `rules_decay()` function.
3. `rules_decay()` in `rules.c`: for each rule where `now - last_reinforced_at > decay_interval` (default 14 days), reduce weight by `decay_amount` (default 5 per interval). Cap minimum at 0.
4. Hard directives (`directive_type='hard'`) decay 3x slower (42 days per interval) to protect critical rules.
5. Rules with `weight < 10` are soft-deleted (marked archived) after 30 days at low weight.
6. `feedback_record()` and `anti_pattern_escalate()` update `last_reinforced_at` on reinforcement.

### Changes

| File | Change |
|------|--------|
| `src/db.c` | Add migration: `last_reinforced_at` column on rules table |
| `src/rules.c` | Implement `rules_decay()` with interval-based weight reduction |
| `src/feedback.c` | Update `last_reinforced_at` in `feedback_record()` |
| `src/memory_advanced.c` | Update `last_reinforced_at` in `anti_pattern_escalate()` |
| `src/cmd_hooks.c` | Call `rules_decay()` during session cleanup |
| `src/headers/rules.h` | Declare `rules_decay()` |

## Acceptance Criteria

- [ ] Rules not reinforced for 14+ days lose 5 weight per interval
- [ ] Hard directive rules decay at 42-day intervals instead of 14
- [ ] Rules with weight < 10 are archived after 30 days at low weight
- [ ] `feedback_record()` resets `last_reinforced_at` on reinforcement
- [ ] `anti_pattern_escalate()` resets `last_reinforced_at` on escalation
- [ ] Existing rows receive `last_reinforced_at = updated_at` via migration
- [ ] Decay runs during session cleanup without user intervention

## Owner and Effort

- **Owner:** aimee
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Database migration adds the column automatically on next session start. Decay runs as part of existing session cleanup.
- **Rollback:** Revert commit plus run `UPDATE rules SET weight = <original>` from backup. The `last_reinforced_at` column is additive and harmless to leave in place.
- **Blast radius:** Rules may weaken faster than expected if `decay_interval` is too short. Default 14 days is conservative. Only affects rule selection weight, not rule content.

## Test Plan

- [ ] Unit tests: decay function with various ages and weights; hard directive slower decay; weight floor at 0
- [ ] Integration tests: create rule, advance time, run decay, verify weight reduced; verify feedback reinforcement resets `last_reinforced_at`
- [ ] Failure injection: interrupted decay (partial UPDATE), concurrent reinforcement during decay
- [ ] Manual verification: create a rule, wait or simulate elapsed time, confirm weight decreases and archival triggers

## Operational Impact

- **Metrics:** Count of rules decayed per session cleanup cycle.
- **Logging:** One log line per cleanup reporting number of rules decayed and number archived.
- **Alerts:** None required.
- **Disk/CPU/Memory:** ~1 UPDATE query per rule per session cleanup. Negligible performance impact.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Time-based rule weight decay | P1 | S | Prevents unbounded rule accumulation, improves signal quality |

## Trade-offs

**Why interval-based (14 days) instead of continuous decay?** Continuous decay requires tracking exact timestamps and computing fractional weights on every access. Interval-based decay is simpler, auditable, and matches the batch-learning model already used in session cleanup.

**Why not just expire old rules?** A rule may be old but still valid. Decay gives it a chance to be reinforced before archival, preserving valuable rules that remain relevant even if they were created long ago.

**Why 5 weight per interval?** At 5 per interval with a starting weight of 100, a completely unreinforced rule takes roughly 280 days (20 intervals) to reach archival threshold. This is conservative enough to avoid premature loss of useful rules.
