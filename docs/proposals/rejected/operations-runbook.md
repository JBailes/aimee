# Proposal: Operations Runbooks

> **Status: Pending.** Previously deferred pending on-demand server — that
> proposal is now done. Ready for implementation.

## Problem

aimee now has multiple deployment modes (standalone monolith, client+server,
webchat systemd service) but no operational documentation for production setup,
monitoring, troubleshooting, or upgrades.

## Goals

- Operators can deploy, monitor, upgrade, and troubleshoot aimee without reading
  source code.
- Incident severity is defined so operators know when to act urgently vs. defer.
- Upgrade procedures are explicit, including client/server version compatibility.

## Approach

### 1. Server Operations Runbook

`docs/operations/server.md`:

- **Installation**: systemd service setup (`systemd/aimee-server.service`),
  socket permissions, log configuration
- **Health checking**: `server.health` JSON-RPC method, socket existence check,
  process monitoring
- **Monitoring**: Prometheus textfile collector metrics. Required metrics:
  - `aimee_server_requests_total` (counter, labels: method, status)
  - `aimee_server_request_duration_ms` (histogram, label: method)
  - `aimee_compute_pool_active` (gauge)
  - `aimee_sessions_active` (gauge)
  - Healthy baselines: request duration p99 < 50ms, active sessions < 20,
    compute pool utilization < 80%
- **Log analysis**: Server stderr output format, common error patterns
- **Troubleshooting**:
  - Server won't start (socket already exists, permission denied, port conflict)
  - Client can't connect (socket missing, UID mismatch, auth failure)
  - High latency (compute pool exhaustion, DB lock contention)
  - Memory/CPU growth (leaked connections, large delegation output)

### 2. Webchat Operations Runbook

`docs/operations/webchat.md`:

- **TLS setup**: Self-signed cert auto-generation (`webchat.c:345`), custom cert
  installation, reverse proxy configuration (nginx/caddy examples)
- **PAM configuration**: Which PAM service file is used
  (`pam_check_credentials()` at `dashboard.c:485`), common auth backends
  (local, LDAP)
- **Session management**: Token lifetime (4 hours), max concurrent sessions,
  cleanup
- **Security hardening**: Bind to localhost only, reverse proxy with real TLS,
  rate limiting
- **Troubleshooting**:
  - Login failures (PAM misconfiguration, user not in group)
  - TLS errors (cert expired, self-signed rejection)
  - Chat not streaming (SSE connection dropped, proxy buffering)

### 3. Backup and Recovery Runbook

`docs/operations/backup.md`:

- **What to back up**: `~/.config/aimee/` (database, config, agents, tokens,
  project descriptions)
- **What NOT to back up**: worktrees (ephemeral), build artifacts
- **Backup script**: Simple cron-based rsync/cp
- **Restore procedure**: Copy files back, run `aimee init` to verify
- **Database recovery**: Link to `migration-recovery` proposal deliverables
  (`aimee db recover`, `aimee db backup`, `aimee db check`)

Note: This section depends on the `migration-recovery` proposal landing first.
Without it, the database recovery section is limited to "restore from file backup."

### 4. Incident Severity Table

Include in `docs/operations/server.md`:

| Severity | Description | Examples | Response |
|----------|-------------|----------|----------|
| SEV1 | Data loss or corruption | Database corruption, backup failure during migration, memory loss | Immediate — stop all sessions, investigate, recover from backup |
| SEV2 | Service degraded | Server unresponsive, hook latency >500ms, webchat login broken | Within 1 hour — restart service, check logs, escalate if no resolution |
| SEV3 | Cosmetic or minor | Stale worktrees accumulating, slow coverage report, dashboard rendering issue | Next maintenance window |

### 5. Upgrade Playbook

Include in `docs/operations/server.md`:

1. Build new binaries (`cd src && make`)
2. Stop active sessions (or wait for them to complete)
3. Stop server: `systemctl stop aimee-server`
4. Replace binaries: `make install` (copies to `/usr/local/bin/`)
5. Start server: `systemctl start aimee-server`
6. Verify: `aimee version` shows new version
7. First session-start will auto-run pending migrations (with backup)

**Version compatibility**: The thin client (`aimee`) and server
(`aimee-server`) must be the same version. Mismatched versions may cause
JSON-RPC method not found errors. Always upgrade both together.

### Changes

| File | Content |
|------|---------|
| `docs/operations/server.md` | Server operations, incident severity, upgrade playbook |
| `docs/operations/webchat.md` | Webchat operations |
| `docs/operations/backup.md` | Backup and recovery |

## Acceptance Criteria

- [ ] Server runbook covers installation, health check, monitoring, and
      troubleshooting with copy-pasteable commands
- [ ] Webchat runbook covers TLS, PAM, and session management
- [ ] Backup runbook covers what to back up, restore procedure, and links to
      `aimee db recover`
- [ ] Incident severity table defines 3 levels with response expectations
- [ ] Upgrade playbook includes version compatibility note
- [ ] Required Prometheus metrics are listed with names, types, and healthy ranges

## Owner and Effort

- **Owner:** TBD
- **Effort:** M (documentation, no code changes)
- **Dependencies:** `migration-recovery` proposal (for database recovery section)

## Rollout and Rollback

- **Rollout:** Merge documentation. No runtime changes.
- **Rollback:** Revert commit. No operational impact.
- **Blast radius:** None — documentation only.

## Test Plan

- [ ] Manual: follow server installation runbook on a clean machine, verify
      aimee-server starts and responds to `server.health`
- [ ] Manual: follow webchat runbook, verify login and chat work
- [ ] Manual: follow backup/restore procedure, verify data survives
- [ ] Manual: follow upgrade playbook, verify version changes and migrations run

## Operational Impact

- **Metrics:** Runbook defines which metrics should exist (not yet implemented
  in code — that's a follow-up).
- **Logging:** No changes.
- **Alerts:** No changes.
- **Disk/CPU/Memory:** No impact.

## Priority

P1 — required before recommending systemd/webchat deployment to others.

## Trade-offs

**Why not auto-generate runbooks from code?** Runbooks include judgment calls
(when to restart vs. investigate, what to back up) that can't be derived from
code. Manual authoring is appropriate for operational documentation.
