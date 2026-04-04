# Proposal: Dynamic Cost-Tier Routing with Ecomode

## Problem

Aimee's delegate router picks the cheapest enabled delegate matching the requested role. This is static — the same tier is used regardless of task complexity. A simple "summarize this file" and a complex "review this architecture for security vulnerabilities" both route to the same cheapest `review` delegate.

This means:
- Simple tasks that could use tier-0 local models are sent to tier-1/2 API delegates when no tier-0 is configured for that role
- Complex tasks that need strong reasoning are initially sent to cheap delegates, fail, and then the primary agent retries or handles it directly — wasting the cheap delegate call
- There is no "ecomode" to globally prefer cheaper delegates when the user wants to minimize cost
- No per-task complexity signal informs routing

oh-my-codex's ecomode demonstrates value in dynamic tier selection: route to LOW by default, escalate to STANDARD/THOROUGH only when the task genuinely requires it, batch similar work, and cap concurrent background operations.

Evidence:
- `agent_coord.c` implements delegate routing with static tier preference
- No task complexity assessment exists
- No global cost mode toggle exists
- Delegate stats track success rates per role/agent but don't feed back into routing decisions

## Goals

- Routing considers task complexity, not just role match and static tier
- An "ecomode" toggle globally biases routing toward cheaper delegates
- Failed delegations at a low tier automatically escalate to a higher tier
- Delegate success history informs routing — delegates that frequently fail for a role get deprioritized
- The user can see cost savings from ecomode vs. normal routing

## Approach

### 1. Task complexity heuristic

Before routing, assess the task prompt for complexity signals:

```c
typedef enum {
    COMPLEXITY_LOW,      // single-file, lookup, format, summarize
    COMPLEXITY_STANDARD, // multi-file, review, refactor
    COMPLEXITY_HIGH,     // architecture, security, reasoning across systems
} task_complexity_t;

task_complexity_t assess_complexity(const char *role, const char *prompt);
```

Heuristics:
- **LOW**: prompt < 200 chars, role is summarize/format/draft, single file mentioned
- **HIGH**: prompt mentions multiple files/systems, role is reason/review with security/architecture keywords, references external systems
- **STANDARD**: everything else

### 2. Complexity-aware routing

Modify the routing logic in `agent_coord.c`:

```
current: cheapest enabled delegate matching role
new:     cheapest enabled delegate matching role AND tier >= min_tier_for_complexity
```

| Complexity | Min tier (normal) | Min tier (ecomode) |
|-----------|-------------------|-------------------|
| LOW | 0 | 0 |
| STANDARD | 0 | 0 |
| HIGH | 1 | 0 (but warn) |

In ecomode, HIGH tasks can still route to tier-0, but if the delegation fails, automatic escalation triggers.

### 3. Automatic tier escalation

When a delegation fails (error response, empty response, or low-confidence result):

```c
int delegate_with_escalation(app_ctx_t *ctx, const char *role, const char *prompt,
                             int starting_tier, int max_tier);
```

1. Try at `starting_tier`
2. If it fails → try at `starting_tier + 1`
3. Continue until success or `max_tier` exceeded
4. Record the successful tier for future routing hints

### 4. Ecomode toggle

```bash
aimee ecomode on    # bias all routing to cheapest delegates
aimee ecomode off   # normal complexity-aware routing
aimee ecomode       # show current state
```

Implementation: a config flag `ecomode` that the router checks. When on:
- All complexity assessments map to min_tier 0
- Automatic escalation still works (so tasks aren't stuck on bad delegates)
- A session-scoped counter tracks "escalations avoided" (cost savings)

### 5. Success-rate feedback

Extend delegate stats to inform routing:

```sql
ALTER TABLE delegate_attempts ADD COLUMN complexity TEXT;
```

After N attempts, if a delegate's success rate for a role+complexity combination drops below 50%, deprioritize it (treat it as tier+1 for routing purposes).

### 6. Cost reporting

```bash
aimee ecomode stats
```

Shows:
- Delegations by tier (how many went to tier-0 vs. higher)
- Escalations (how many times a cheap delegate failed and a higher-tier succeeded)
- Estimated savings (based on tier cost ratios)

### Changes

| File | Change |
|------|--------|
| `src/agent_coord.c` | Add complexity assessment, complexity-aware routing, tier escalation |
| `src/config.c` | Add `ecomode` config flag |
| `src/cmd_core.c` | Add `ecomode` subcommand |
| `src/agent_eval.c` | Extend delegate stats with complexity tracking |
| `src/tests/test_routing.c` | Tests for complexity-aware routing, escalation, ecomode |

## Acceptance Criteria

- [ ] `assess_complexity()` classifies tasks into LOW/STANDARD/HIGH
- [ ] Routing considers complexity — HIGH tasks don't route to tier-0 (unless ecomode)
- [ ] Failed delegations automatically escalate to the next tier
- [ ] `aimee ecomode on/off` toggles cost-biased routing
- [ ] `aimee ecomode stats` shows delegation tier distribution and escalation count
- [ ] Success-rate feedback deprioritizes unreliable delegates

## Owner and Effort

- **Owner:** aimee
- **Effort:** M (3-4 focused sessions)
- **Dependencies:** None (extends existing routing in agent_coord.c)

## Rollout and Rollback

- **Rollout:** Routing changes are additive — existing cheapest-match behavior is the default when ecomode is off and complexity is STANDARD.
- **Rollback:** Revert commit. Routing reverts to static cheapest-match.
- **Blast radius:** Low. Routing changes affect all delegations but fallback to existing behavior.

## Test Plan

- [ ] Unit tests: complexity assessment for various prompt/role combinations
- [ ] Unit tests: routing with complexity constraints — correct tier selection
- [ ] Unit tests: tier escalation — fail at tier-0, succeed at tier-1
- [ ] Unit tests: ecomode override — all tasks start at tier-0
- [ ] Integration tests: end-to-end delegation with escalation
- [ ] Manual verification: enable ecomode, observe tier-0 preference with escalation on failure

## Operational Impact

- **Metrics:** `delegations_by_tier{tier}`, `delegation_escalations`, `ecomode_active`
- **Logging:** Routing decisions: `aimee: delegate [review]: complexity=HIGH, tier=1 (ecomode: off)`
- **Alerts:** None
- **Disk/CPU/Memory:** Negligible. Complexity assessment is a string scan. One extra column per attempt.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Complexity assessment | P1 | S | High — enables smarter routing |
| Complexity-aware routing | P1 | S | High — core value |
| Automatic tier escalation | P1 | S | High — graceful failure recovery |
| Ecomode toggle | P2 | S | Medium — user control |
| Success-rate feedback | P2 | S | Medium — self-improving routing |
| Cost reporting | P3 | S | Low — visibility |

## Trade-offs

**Why heuristic complexity instead of delegate-assessed complexity?**
Calling a delegate to assess complexity before routing adds latency and cost. A simple heuristic (prompt length, role, keyword presence) is fast and sufficient for tier selection. The escalation mechanism catches cases where the heuristic underestimates.

**Why not add more tiers?**
The existing 4 tiers (0-3) are sufficient. The improvement is in routing *logic*, not tier granularity. Adding tiers would require users to reconfigure delegates.

**Why deprioritize instead of disable unreliable delegates?**
A delegate might be unreliable for complex tasks but fine for simple ones. Deprioritization (treat as tier+1) lets it still handle tasks at its actual capability level while being bypassed for tasks that exceed it.
