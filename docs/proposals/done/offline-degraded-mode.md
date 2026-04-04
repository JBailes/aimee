# Proposal: Offline / Degraded Mode Awareness

## Problem

When the AI provider is unreachable (network outage, API key expired, rate
limited), aimee commands that depend on the provider fail with raw HTTP errors.
Meanwhile, many commands work perfectly without a provider: memory search/store,
index find/scan, rules, worktree management, db commands, dashboard.

There is no clear indication of provider status and no graceful degradation
messaging. Users see confusing errors like "connection refused" or "401
unauthorized" without guidance on what still works.

## Goals

- Clear indication of provider availability.
- Provider-dependent commands produce actionable error messages when offline.
- Local-only commands work without any provider check overhead.

## Approach

### 1. Provider health state

Cache provider reachability in memory (not on every command — just when needed):

```c
typedef struct {
    int available;          /* 1=ok, 0=unreachable, -1=unknown */
    int64_t last_check_ms;  /* monotonic timestamp of last check */
    char error[256];        /* last error message */
} provider_health_t;
```

Health is checked:
- On first provider-dependent command in a session
- When a provider call fails (update cached state)
- On `aimee agent test-provider` (explicit check)

Health is NOT checked:
- On local-only commands (memory, index, rules, db, worktree)
- On every command invocation (no startup latency hit)

### 2. Actionable error messages

When a provider-dependent command fails due to provider unavailability:

```
aimee: provider 'claude' is unreachable (connection refused).
Local commands (memory, index, rules, db) still work.
Run 'aimee agent test-provider' for diagnostics.
```

Distinguish error classes:
- Network unreachable → "check network connection"
- Auth failure (401/403) → "check API key: aimee agent test-provider"
- Rate limited (429) → "rate limited, retry in Ns"
- Server error (5xx) → "provider error, retry later"

### 3. Status command

`aimee status` shows system health at a glance:

```
aimee v0.9.3
Database:  ok (schema v34, 1247 pages)
Provider:  claude (ok, 320ms latency)
Server:    running (pid 12345, 3 connections)
Worktrees: 2 active, 0 stale
```

When provider is down:

```
Provider:  claude (unreachable — connection refused)
```

### Changes

| File | Change |
|------|--------|
| `src/agent_http.c` | Add `provider_health_t` caching, classify HTTP errors |
| `src/cmd_core.c` | Add `aimee status` subcommand |
| `src/agent.c` | Check provider health before delegation, produce actionable errors |
| `src/cmd_table.c` | Register `status` subcommand |

## Acceptance Criteria

- [ ] Provider health is cached and only checked when needed
- [ ] Local-only commands have zero provider check overhead
- [ ] Provider failures produce actionable error messages with next steps
- [ ] Error messages distinguish network, auth, rate-limit, and server errors
- [ ] `aimee status` shows provider health alongside other system info

## Owner and Effort

- **Owner:** TBD
- **Effort:** S
- **Dependencies:** None

## Rollout and Rollback

- **Rollout:** Ships with next binary. Error messages improve immediately.
- **Rollback:** Revert commit. Raw HTTP errors return.
- **Blast radius:** None — error messages change, behavior does not.

## Test Plan

- [ ] Unit test: health cache returns cached result within TTL
- [ ] Unit test: HTTP error classification produces correct error class
- [ ] Integration test: delegation with unreachable provider shows actionable error
- [ ] Integration test: `aimee status` shows provider health
- [ ] Manual: disconnect network, verify local commands work and provider commands show clear error

## Operational Impact

- **Metrics:** None.
- **Logging:** Provider state changes logged (available→unreachable, unreachable→available).
- **Alerts:** None.
- **Disk/CPU/Memory:** One cached health struct in memory. Negligible.

## Priority

P2 — quality-of-life improvement for error clarity.

## Trade-offs

**Why not automatic provider failover?** aimee supports multiple providers, but
automatic failover requires policy decisions (which provider to fall back to,
cost implications, capability differences). Manual provider selection is simpler
and more predictable for a local tool.

**Why not check health on every startup?** Adds latency to every command for a
check that is only useful when the provider is actually needed. Lazy checking
keeps startup fast.
